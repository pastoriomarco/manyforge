#!/usr/bin/env python3
"""Strict verifier for the remaining local Step 18 performance proof cells."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

from source_identity import (
    SourceIdentityError,
    source_identity,
    specs_contract_identity,
    validate_identity,
    validate_specs_contract_identity,
)


ARTIFACT_SCHEMA = "manyforge.step18.proof_cell_artifact.v2"
CONTACT_SCHEMA = "manyforge.step18.validation_nonregression.v1"
POLICY_SCHEMA = "manyforge.step18.policy_support_perf.v1"
PRECISE_REFERENCE_SCHEMA = "manyforge.step18.precise_reference_parity.v1"
PRECISE_REFERENCE_CORPUS_SCHEMA = (
    "manyforge.step18.precise_reference_corpus.v1"
)
PRECISE_REFERENCE_NORMALIZATION_ID = (
    "step18_precise_reference_normalization.v1"
)
PRECISE_REFERENCE_PAYLOAD_KEYS = frozenset(
    {
        "binary_sha256",
        "build_type",
        "clean_checkout_manifest_sha256",
        "compiler",
        "compiler_flags",
        "corpus_sha256",
        "cpu_frequency_driver_set",
        "cpu_governor_set",
        "effective_cpu_affinity",
        "image_id",
        "kernel_id",
        "measured_runs",
        "measurement_profile_usage",
        "passed",
        "process_instance_id",
        "process_nice",
        "qualification_patch_sha256",
        "record_type",
        "reference_evidence",
        "requested_cpu_affinity",
        "run_id",
        "scheduler_policy",
        "scheduler_priority",
        "schema_version",
        "source_identity",
        "source_role",
        "specs_contract_identity",
        "specs_source_identity",
        "timing_gate_excluded",
        "warmup_runs",
    }
)
SUMMARY_SCHEMA = "manyforge.step18.proof_cell_verification.v2"
CLEAN_SCHEMA = "manyforge.step18.validation_nonregression_clean_checkout.v2"
BASELINE_SOURCE = "2bfe30eeb37952f1ddc36bfdbf89d471a9f6b909"
BASELINE_SPECS = "67c24447a117e14c8274b13d05d720766671aa7a"
BASELINE_ROLE = "pristine_baseline_timing"
CURRENT_ROLE = "current_candidate"
LIGHTWEIGHT_FIXTURE = "ur5_support_contact_release"
CONTACT_FIXTURE = "step18_contact_grasp_envelope_v1"
POLICY_FIXTURE = "ur5_support_contact_release"
POLICY_VARIANT = "dense_far_pair_distractor_extension_v1"
PRECISE_REFERENCE_CASE_ORDER = (
    "decreasing_then_separated_pass",
    "absent_configured_pair_no_fabricated_metric_pass",
    "initial_penetration_exceeds_policy",
    "penetration_increased",
    "insufficient_release_progress",
    "colliding_configured_metric_missing",
    "new_configured_contact_after_clear_start",
    "equal_count_unconfigured_pair_substitution",
    "recontact_after_release",
    "final_sample_not_separated",
)
PRECISE_REFERENCE_WORKERS = (1, 2, 3, 4, 5)
PRECISE_REFERENCE_PARTITION_WORKERS = (2, 3, 4, 5)
PRECISE_REFERENCE_COMPLETION_ORDERS = (
    (0, 1, 2),
    (0, 2, 1),
    (1, 0, 2),
    (1, 2, 0),
    (2, 0, 1),
    (2, 1, 0),
)
PRECISE_REFERENCE_COMPLETION_CLEARANCES = (
    -0.050,
    -0.040,
    -0.030,
    -0.020,
    -0.030,
    -0.020,
    -0.010,
    0.001,
    0.002,
    0.003,
    0.004,
    0.005,
)
PRECISE_CONFIGURED_PAIR = (
    "object/step18_reference_part/shape/0",
    "robot/step18_reference_tool/collision/0",
)
PRECISE_UNCONFIGURED_PAIR = (
    "fixture/step18_reference_table/shape/0",
    "robot/step18_reference_elbow/collision/0",
)
PRECISE_FAR_PAIR = (
    "fixture/step18_reference_far/shape/0",
    "robot/step18_reference_tool/collision/0",
)
PRECISE_LEGACY_DIAGNOSTIC = (
    "contact_release_min_" + "separation_clearance_m"
)
PRECISE_GLOBAL_VOLATILE_DIAGNOSTICS = (
    "snapshot_sync_duration_ns",
    "total_call_duration_ns",
)
WAYPOINTS = (96, 500)
LIGHTWEIGHT_WAYPOINTS = (96, 500, 1000)
POLICY_WAYPOINTS = (96, 500, 1000)
BASELINE_FREE_SPACE_FIXTURES = (
    "primitive_dense_ur5_gripper",
    "cylinder_dense_ur5_gripper",
    "attached_object_ur5_gripper",
    "isaac_like_primitive_cell",
)
CURRENT_FREE_SPACE_FIXTURES = (
    "primitive_dense_ur5_gripper",
    "runtime_capacity_envelope",
    "cylinder_dense_ur5_gripper",
    "attached_object_ur5_gripper",
    "unsupported_mesh_fail_closed",
    "isaac_like_primitive_cell",
    "support_contact_ordered_summary",
    "binary_contact_ordered_summary",
    "bounded_penetration_insertion",
)
WARMUP_RUNS = 5
MEASURED_RUNS = 50
CONTACT_WORKER_PROFILES = (1, 4)
LIGHTWEIGHT_RATIO_LIMIT = 1.25
NONREGRESSION_RATIO_LIMIT = 1.05
POLICY_DISTANCE_CPU_RATIO_LIMIT = 0.50
POLICY_BINARY_CPU_RATIO_LIMIT = 0.25
POLICY_DISTANCE_REDUCTION_MINIMUM = 0.95
UUID = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-"
    r"[89ab][0-9a-f]{3}-[0-9a-f]{12}"
)
LEGACY_SOURCE_FIELDS = {
    "source_commit",
    "specs_commit",
    "specs_candidate_sha256",
    "working_tree_candidate_sha256",
}


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


def canonical_sha256(value: Any) -> str:
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def sha256_string(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(char in "0123456789abcdef" for char in value)
    )


def fingerprint_string(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 16
        and all(char in "0123456789abcdef" for char in value)
    )


def positive_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def source_identity_object(value: Any) -> bool:
    try:
        validate_identity(value)
    except SourceIdentityError:
        return False
    return True


def specs_contract_identity_object(value: Any) -> bool:
    try:
        validate_specs_contract_identity(value)
    except SourceIdentityError:
        return False
    return True


def four_core_affinity(value: Any) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        cpus: set[int] = set()
        for part in value.split(","):
            if "-" in part:
                first_text, last_text = part.split("-", 1)
                first, last = int(first_text), int(last_text)
                if first > last:
                    return False
                cpus.update(range(first, last + 1))
            else:
                cpus.add(int(part))
        if len(cpus) != 4 or min(cpus) < 0:
            return False
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
        return value == ",".join(ranges)
    except (ValueError, IndexError):
        return False


def finite_positive(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value > 0
    )


def finite_nonnegative(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value >= 0
    )


def nested(value: Any, *path: str, default: Any = None) -> Any:
    current = value
    for key in path:
        if not isinstance(current, dict) or key not in current:
            return default
        current = current[key]
    return current


def exact_object(
    value: Any, keys: Iterable[str], label: str
) -> dict[str, Any]:
    expected = set(keys)
    if not isinstance(value, dict) or set(value) != expected:
        observed = sorted(value) if isinstance(value, dict) else type(value).__name__
        raise ValueError(
            f"{label} keys are not exact: expected={sorted(expected)} "
            f"observed={observed}"
        )
    return value


def exact_list(value: Any, length: int, label: str) -> list[Any]:
    if not isinstance(value, list) or len(value) != length:
        observed = len(value) if isinstance(value, list) else type(value).__name__
        raise ValueError(
            f"{label} must contain exactly {length} items; observed={observed}"
        )
    return value


def canonical_pair(value: Any, label: str) -> tuple[str, str]:
    pair = exact_object(value, ("body_a", "body_b"), label)
    body_a, body_b = pair["body_a"], pair["body_b"]
    if not (
        isinstance(body_a, str)
        and body_a
        and isinstance(body_b, str)
        and body_b
        and body_a < body_b
    ):
        raise ValueError(f"{label} is not a canonical non-empty pair")
    return body_a, body_b


def _precise_sample(
    configured_contact: bool,
    clearance: float | None,
    unconfigured_contact: bool = False,
) -> dict[str, Any]:
    pairs: list[tuple[str, str]] = []
    if configured_contact:
        pairs.append(PRECISE_CONFIGURED_PAIR)
    if unconfigured_contact:
        pairs.append(PRECISE_UNCONFIGURED_PAIR)
    return {
        "colliding_pairs": sorted(pairs),
        "configured_signed_clearance_m": clearance,
    }


def precise_reference_case_inputs() -> dict[str, dict[str, Any]]:
    cases: dict[str, dict[str, Any]] = {}

    def add(
        case_id: str,
        samples: list[dict[str, Any]],
        require_separation: bool,
        require_decrease: bool,
        max_start: float = 0.005,
        min_progress: float = 0.0,
        allow_recontact: bool = False,
    ) -> None:
        cases[case_id] = {
            "policy": {
                "mode": "contact_release",
                "release_evidence_mode": "signed_penetration",
                "enforce_max_start_penetration": True,
                "max_start_penetration_m": max_start,
                "release_tolerance_m": 1e-5,
                "min_release_progress_m": min_progress,
                "require_separation_by_end": require_separation,
                "require_strictly_decreasing_penetration": require_decrease,
                "allow_recontact": allow_recontact,
            },
            "configured_pair": PRECISE_CONFIGURED_PAIR,
            "samples": samples,
        }

    add(
        "decreasing_then_separated_pass",
        [
            _precise_sample(True, -0.004),
            _precise_sample(True, -0.003),
            _precise_sample(True, -0.001),
            _precise_sample(False, None),
            _precise_sample(False, None),
        ],
        True,
        True,
    )
    add(
        "absent_configured_pair_no_fabricated_metric_pass",
        [_precise_sample(False, None) for _ in range(5)],
        True,
        True,
    )
    add(
        "initial_penetration_exceeds_policy",
        [
            _precise_sample(True, -0.006),
            _precise_sample(True, -0.003),
            _precise_sample(True, -0.001),
            _precise_sample(False, None),
            _precise_sample(False, None),
        ],
        True,
        True,
    )
    add(
        "penetration_increased",
        [
            _precise_sample(True, -0.004),
            _precise_sample(True, -0.003),
            _precise_sample(True, -0.006),
            _precise_sample(False, None),
            _precise_sample(False, None),
        ],
        True,
        True,
        max_start=0.010,
    )
    add(
        "insufficient_release_progress",
        [_precise_sample(True, -0.004) for _ in range(5)],
        False,
        True,
        min_progress=0.001,
    )
    add(
        "colliding_configured_metric_missing",
        [
            _precise_sample(True, -0.003),
            _precise_sample(True, None),
            _precise_sample(True, -0.001),
            _precise_sample(False, None),
            _precise_sample(False, None),
        ],
        True,
        True,
    )
    add(
        "new_configured_contact_after_clear_start",
        [
            _precise_sample(False, None),
            _precise_sample(True, -0.002),
            _precise_sample(False, None),
            _precise_sample(False, None),
            _precise_sample(False, None),
        ],
        False,
        False,
    )
    add(
        "equal_count_unconfigured_pair_substitution",
        [
            _precise_sample(True, -0.003),
            _precise_sample(False, None, True),
            _precise_sample(False, None),
            _precise_sample(False, None),
            _precise_sample(False, None),
        ],
        True,
        True,
    )
    add(
        "recontact_after_release",
        [
            _precise_sample(True, -0.003),
            _precise_sample(False, None),
            _precise_sample(True, -0.001),
            _precise_sample(False, None),
            _precise_sample(False, None),
        ],
        False,
        False,
    )
    add(
        "final_sample_not_separated",
        [
            _precise_sample(True, -0.004),
            _precise_sample(True, -0.003),
            _precise_sample(True, -0.002),
            _precise_sample(True, -0.001),
            _precise_sample(True, -0.0005),
        ],
        True,
        False,
    )
    return cases


PRECISE_REFERENCE_EXPECTED_DECISIONS = {
    "decreasing_then_separated_pass": (True, 0, 0, None, None),
    "absent_configured_pair_no_fabricated_metric_pass": (
        True,
        0,
        0,
        None,
        None,
    ),
    "initial_penetration_exceeds_policy": (False, 1, 2, 0, 0),
    "penetration_increased": (False, 1, 2, 2, 20_000_000),
    "insufficient_release_progress": (False, 1, 2, 4, 40_000_000),
    "colliding_configured_metric_missing": (False, 3, 2, 1, 10_000_000),
    "new_configured_contact_after_clear_start": (
        False,
        1,
        2,
        1,
        10_000_000,
    ),
    "equal_count_unconfigured_pair_substitution": (
        False,
        1,
        2,
        1,
        10_000_000,
    ),
    "recontact_after_release": (False, 1, 2, 2, 20_000_000),
    "final_sample_not_separated": (False, 1, 2, 4, 40_000_000),
}


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
            or fields[2][2:] != fields[3][2:]
        ):
            raise ValueError(f"invalid qualification patch header: {line}")
        paths.add(fields[2][2:])
    if not paths:
        raise ValueError("qualification patch contains no paths")
    return paths


def load_artifact(path: Path) -> dict[str, Any]:
    payload, digest = read_file_snapshot(path)
    parsed = json.loads(payload)
    if not isinstance(parsed, dict):
        raise ValueError(f"artifact must be one JSON object: {path}")
    parsed["_artifact"] = {
        "path": str(path.resolve()),
        "sha256": digest,
    }
    return parsed


def require_artifacts_unchanged(
    artifact_manifest: list[dict[str, str]],
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


def validated_evidence_output(
    output: Path,
    repository_root: Path,
    current_source_root: Path | None,
    protected_checkout_roots: Iterable[Path],
) -> Path:
    destination = output.expanduser().resolve(strict=False)
    if destination.exists():
        raise ValueError(f"refusing to overwrite verification summary: {destination}")
    for root in protected_checkout_roots:
        resolved_root = root.resolve()
        if current_source_root is not None and resolved_root == current_source_root:
            continue
        if path_within(destination, resolved_root):
            raise ValueError(
                f"verification output must not be inside checkout {resolved_root}"
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


def record_label(record: dict[str, Any]) -> str:
    return f"{nested(record, 'cell')}:{nested(record, 'identity', 'run_id')}"


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
) -> tuple[Path | None, set[Path]]:
    current_records = [
        record
        for record in records
        if nested(record, "identity", "source_role") == CURRENT_ROLE
    ]
    current_source_roots = checkout_roots_from_records(
        current_records, "source_root"
    )
    current_specs_roots = checkout_roots_from_records(
        current_records, "specs_root"
    )
    all_roots = checkout_roots_from_records(records, "source_root")
    all_roots.update(checkout_roots_from_records(records, "specs_root"))
    current_source_root = (
        next(iter(current_source_roots))
        if len(current_source_roots) == 1
        else None
    )
    current_specs_root = (
        next(iter(current_specs_roots))
        if len(current_specs_roots) == 1
        else None
    )
    checks.require(
        "current_candidate_checkout_roots",
        current_source_root == repository_root.resolve()
        and current_specs_root is not None,
        f"source_roots={sorted(map(str, current_source_roots))} "
        f"specs_roots={sorted(map(str, current_specs_roots))}",
    )
    if current_source_root is None or current_specs_root is None:
        return current_source_root, all_roots
    expected_source = nested(
        current_records[0], "identity", "source_identity", default={}
    ) if current_records else {}
    expected_specs = nested(
        current_records[0], "identity", "specs_source_identity", default={}
    ) if current_records else {}
    expected_contract = nested(
        current_records[0], "identity", "specs_contract_identity", default={}
    ) if current_records else {}
    try:
        observed_source = source_identity(current_source_root)
        observed_specs = source_identity(current_specs_root)
        observed_contract = specs_contract_identity(current_specs_root)
        detail = ""
    except (OSError, SourceIdentityError) as error:
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
    return current_source_root, all_roots


def resolve_summary_destination(
    output: Path | None,
    records: list[dict[str, Any]],
    repository_root: Path,
) -> Path | None:
    if output is None:
        return None
    current_records = [
        record
        for record in records
        if nested(record, "identity", "source_role") == CURRENT_ROLE
    ]
    current_roots = checkout_roots_from_records(current_records, "source_root")
    current_root = next(iter(current_roots)) if len(current_roots) == 1 else None
    protected = checkout_roots_from_records(records, "source_root")
    protected.update(checkout_roots_from_records(records, "specs_root"))
    return validated_evidence_output(
        output, repository_root, current_root, protected
    )


def publication_checkout_snapshot(
    records: list[dict[str, Any]], repository_root: Path
) -> dict[str, Any]:
    current_records = [
        record
        for record in records
        if nested(record, "identity", "source_role") == CURRENT_ROLE
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
                "source_identity": source_identity(source_root),
                "specs_source_identity": source_identity(specs_root),
                "specs_contract_identity": specs_contract_identity(specs_root),
                "repository_root_matches": source_root == repository_root.resolve(),
            }
        )
    except (OSError, SourceIdentityError) as error:
        result["error"] = f"{type(error).__name__}: {error}"
    return result


def publish_summary(
    destination: Path,
    payload: bytes,
    records: list[dict[str, Any]],
    repository_root: Path,
    artifact_manifest: list[dict[str, Any]],
    tool_manifest: list[dict[str, Any]],
) -> None:
    checkout_before = publication_checkout_snapshot(records, repository_root)
    staged = stage_output(destination, payload)
    try:
        if hashlib.sha256(staged.read_bytes()).digest() != hashlib.sha256(
            payload
        ).digest():
            raise ValueError("staged verification summary changed before publication")
        require_manifest_snapshot(artifact_manifest, "retained artifact")
        require_manifest_snapshot(tool_manifest, "retained tool")
        if publication_checkout_snapshot(records, repository_root) != checkout_before:
            raise ValueError(
                "current candidate or Specs identity changed during summary publication"
            )
        if resolve_summary_destination(
            destination, records, repository_root
        ) != destination:
            raise ValueError("verification output destination changed")
        publish_staged_exclusive(staged, destination)
    finally:
        staged.unlink(missing_ok=True)


def median(values: Iterable[float]) -> float:
    materialized = [
        float(value)
        for value in values
        if isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
    ]
    return float(statistics.median(materialized)) if materialized else math.nan


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", nargs="+", type=Path)
    parser.add_argument("--minimum-runs", type=int, default=3)
    parser.add_argument(
        "--development-smoke",
        action="store_true",
        help="admit partial one-process policy-cell smoke input; never closes the formal gate",
    )
    parser.add_argument(
        "--policy-support-only",
        action="store_true",
        help="verify only the same-geometry policy-support cell",
    )
    parser.add_argument("--baseline-clean-checkout-manifest", type=Path, required=True)
    parser.add_argument("--baseline-qualification-patch", type=Path, required=True)
    parser.add_argument("--summary-out", type=Path)
    return parser.parse_args()


def validate_envelope(record: dict[str, Any], checks: Checks) -> None:
    label = record_label(record)
    identity_value = record.get("identity", {})
    provenance_value = record.get("provenance", {})
    identity = identity_value if isinstance(identity_value, dict) else {}
    provenance = provenance_value if isinstance(provenance_value, dict) else {}
    payload = record.get("payload")
    source_identity = identity.get("source_identity")
    specs_source_identity = identity.get("specs_source_identity")
    specs_contract_identity = identity.get("specs_contract_identity")
    checks.require(f"{label}:schema", record.get("schema_version") == ARTIFACT_SCHEMA)
    checks.require(
        f"{label}:cell",
        record.get("cell")
        in {
            "lightweight_collision_only",
            "policy_support_contact",
            "precise_reference_parity",
            "contact_grasp",
            "free_space",
        },
    )
    checks.require(f"{label}:payload_object", isinstance(payload, dict))
    checks.require(
        f"{label}:payload_hash",
        isinstance(payload, dict)
        and provenance.get("payload_canonical_sha256")
        == canonical_sha256(payload),
    )
    checks.require(
        f"{label}:structured_source_identities",
        source_identity_object(source_identity)
        and source_identity_object(specs_source_identity)
        and specs_contract_identity_object(specs_contract_identity)
        and not (LEGACY_SOURCE_FIELDS & set(identity))
        and isinstance(payload, dict)
        and not (LEGACY_SOURCE_FIELDS & set(payload))
        and payload.get("source_identity") == source_identity
        and payload.get("specs_source_identity") == specs_source_identity
        and payload.get("specs_contract_identity") == specs_contract_identity
        and isinstance(specs_source_identity, dict)
        and isinstance(specs_contract_identity, dict)
        and specs_contract_identity.get("head_commit")
        == specs_source_identity.get("head_commit")
        and specs_contract_identity.get("head_tree")
        == specs_source_identity.get("head_tree"),
    )
    checks.require(
        f"{label}:run_process_and_affinity",
        isinstance(identity.get("run_id"), str)
        and bool(identity.get("run_id"))
        and isinstance(identity.get("process_instance_id"), str)
        and UUID.fullmatch(identity.get("process_instance_id")) is not None
        and isinstance(payload, dict)
        and payload.get("process_instance_id") == identity.get("process_instance_id")
        and four_core_affinity(identity.get("requested_cpu_affinity"))
        and sha256_string(identity.get("image_id"))
        and sha256_string(identity.get("binary_sha256")),
    )
    checks.require(
        f"{label}:frozen_identity_expectations",
        isinstance(source_identity, dict)
        and isinstance(specs_source_identity, dict)
        and isinstance(specs_contract_identity, dict)
        and provenance.get("expected_source_fingerprint_sha256")
        == source_identity.get("fingerprint_sha256")
        and provenance.get("expected_specs_source_fingerprint_sha256")
        == specs_source_identity.get("fingerprint_sha256")
        and provenance.get("expected_specs_contract_fingerprint_sha256")
        == specs_contract_identity.get("contract_fingerprint_sha256"),
    )
    checks.require(
        f"{label}:fixed_measurement_profile",
        provenance.get("measurement_profile")
        == {"warmup_runs": WARMUP_RUNS, "measured_runs": MEASURED_RUNS},
    )
    command = provenance.get("command_argv")
    checks.require(
        f"{label}:exact_command",
        isinstance(command, list)
        and len(command) >= 4
        and command[0:2] == ["taskset", "-c"]
        and command[2] == identity.get("requested_cpu_affinity")
        and all(isinstance(value, str) and value for value in command),
    )
    runner_command = provenance.get("runner_command_argv")
    build_command = provenance.get("build_command_argv")
    outer_command = provenance.get("outer_command_argv")
    runner_environment = provenance.get("runner_environment")
    checks.require(
        f"{label}:script_and_outer_provenance",
        sha256_string(provenance.get("runner_script_sha256"))
        and sha256_string(provenance.get("wrapper_script_sha256"))
        and sha256_string(provenance.get("source_identity_helper_sha256"))
        and sha256_string(provenance.get("raw_payload_sha256"))
        and all(
            isinstance(argv, list)
            and len(argv) >= 2
            and all(isinstance(item, str) and item for item in argv)
            for argv in (runner_command, build_command, outer_command)
        )
        and isinstance(runner_environment, dict)
        and runner_environment.get("requested_cpu_affinity")
        == identity.get("requested_cpu_affinity")
        and runner_environment.get("image_id") == identity.get("image_id")
        and runner_environment.get("warmup_runs") == WARMUP_RUNS
        and runner_environment.get("measured_runs") == MEASURED_RUNS
        and runner_environment.get("build_type") == "Release"
        and positive_integer(runner_environment.get("build_jobs"))
        and isinstance(runner_environment.get("compiler"), str)
        and bool(runner_environment.get("compiler"))
        and runner_environment.get("compiler_flags")
        == "-Wno-error=maybe-uninitialized"
        and isinstance(runner_environment.get("cpu_governor_set"), str)
        and bool(runner_environment.get("cpu_governor_set"))
        and isinstance(runner_environment.get("cpu_frequency_driver_set"), str)
        and bool(runner_environment.get("cpu_frequency_driver_set"))
        and isinstance(runner_environment.get("kernel_id"), str)
        and bool(runner_environment.get("kernel_id"))
        and isinstance(runner_environment.get("source_root"), str)
        and bool(runner_environment.get("source_root"))
        and isinstance(runner_environment.get("specs_root"), str)
        and bool(runner_environment.get("specs_root"))
        and runner_environment.get("source_identity_helper_sha256")
        == provenance.get("source_identity_helper_sha256")
        and build_command[:2] == ["docker", "compose"]
        and outer_command[:2] == ["docker", "compose"],
    )
    role = identity.get("source_role")
    if role == BASELINE_ROLE:
        checks.require(
            f"{label}:pinned_pristine_source",
            isinstance(source_identity, dict)
            and isinstance(specs_source_identity, dict)
            and isinstance(specs_contract_identity, dict)
            and source_identity.get("head_commit") == BASELINE_SOURCE
            and source_identity.get("dirty") is True
            and specs_source_identity.get("head_commit") == BASELINE_SPECS
            and specs_source_identity.get("dirty") is False
            and specs_contract_identity.get("head_commit") == BASELINE_SPECS
            and specs_contract_identity.get("contract_dirty") is False
            and provenance.get("checkout_base_clean") is True
            and sha256_string(provenance.get("clean_checkout_manifest_sha256"))
            and sha256_string(provenance.get("qualification_patch_sha256")),
        )
    elif role == CURRENT_ROLE:
        checks.require(
            f"{label}:current_candidate_source",
            provenance.get("checkout_base_clean") is False
            and provenance.get("clean_checkout_manifest_sha256") is None
            and provenance.get("qualification_patch_sha256") is None
            and isinstance(source_identity, dict)
            and isinstance(specs_source_identity, dict)
            and isinstance(specs_contract_identity, dict),
        )
    else:
        checks.require(f"{label}:known_source_role", False, f"role={role}")


def _precise_revision(
    value: Any, label: str, *, nullable: bool = True
) -> dict[str, Any] | None:
    if value is None and nullable:
        return None
    revision = exact_object(value, ("revision_id", "content_hash"), label)
    if (
        not isinstance(revision["revision_id"], int)
        or isinstance(revision["revision_id"], bool)
        or revision["revision_id"] < 0
        or not isinstance(revision["content_hash"], str)
        or not revision["content_hash"]
    ):
        raise ValueError(f"{label} is not a valid collision-world revision")
    return dict(revision)


def _precise_decision(value: Any, label: str) -> dict[str, Any]:
    decision = exact_object(
        value,
        (
            "accepted",
            "action_code",
            "severity_code",
            "first_failing_waypoint",
            "first_failing_time_ns",
            "collision_world_revision",
            "failure_archive_id",
        ),
        label,
    )
    if not isinstance(decision["accepted"], bool):
        raise ValueError(f"{label}.accepted must be boolean")
    for field in ("action_code", "severity_code"):
        if not isinstance(decision[field], int) or isinstance(
            decision[field], bool
        ):
            raise ValueError(f"{label}.{field} must be an integer")
    for field in ("first_failing_waypoint", "first_failing_time_ns"):
        item = decision[field]
        if item is not None and (
            not isinstance(item, int) or isinstance(item, bool) or item < 0
        ):
            raise ValueError(f"{label}.{field} must be null or nonnegative")
    collision_world_revision = _precise_revision(
        decision["collision_world_revision"],
        f"{label}.collision_world_revision",
    )
    failure_archive_id = decision["failure_archive_id"]
    if failure_archive_id is not None and not (
        isinstance(failure_archive_id, str) and failure_archive_id
    ):
        raise ValueError(f"{label}.failure_archive_id is invalid")
    return {
        **decision,
        "collision_world_revision": collision_world_revision,
    }


def _precise_stable_result_fields(
    record: dict[str, Any], label: str
) -> dict[str, Any]:
    for field in (
        "query_id",
        "policy_revision",
        "policy_revision_used",
        "contact_body_a",
        "contact_body_b",
    ):
        if record[field] is not None and not (
            isinstance(record[field], str) and record[field]
        ):
            raise ValueError(f"{label}.{field} is invalid")
    for field in ("surface_code", "sampling_mode_code"):
        if not isinstance(record[field], int) or isinstance(record[field], bool):
            raise ValueError(f"{label}.{field} is invalid")
    if record["state_time_ns"] is not None and (
        not isinstance(record["state_time_ns"], int)
        or isinstance(record["state_time_ns"], bool)
    ):
        raise ValueError(f"{label}.state_time_ns is invalid")
    if not (
        isinstance(record["engine_id"], str)
        and isinstance(record["engine_version"], str)
        and record["query_duration_ns_omitted_as_volatile"] is True
    ):
        raise ValueError(f"{label} engine/volatile result fields are invalid")
    return {
        "query_id": record["query_id"],
        "collision_world_revision": _precise_revision(
            record["collision_world_revision"],
            f"{label}.collision_world_revision",
        ),
        "policy_revision": record["policy_revision"],
        "policy_revision_used": record["policy_revision_used"],
        "surface_code": record["surface_code"],
        "state_time_ns": record["state_time_ns"],
        "engine_id": record["engine_id"],
        "engine_version": record["engine_version"],
        "sampling_mode_code": record["sampling_mode_code"],
        "contact_body_a": record["contact_body_a"],
        "contact_body_b": record["contact_body_b"],
    }


def _precise_normalize_diagnostics(
    value: Any,
    role: str,
    audit: dict[str, int],
    label: str,
) -> dict[str, str]:
    if not isinstance(value, dict) or not all(
        isinstance(key, str) and key
        and isinstance(item, str)
        for key, item in value.items()
    ):
        raise ValueError(f"{label} must be a string-to-string object")
    diagnostics = dict(value)
    for key in PRECISE_GLOBAL_VOLATILE_DIAGNOSTICS:
        if key in diagnostics:
            raise ValueError(
                f"{label} retained globally volatile diagnostic {key}"
            )
    is_contact_release_violation = "contact_release_violation" in diagnostics
    legacy_present = PRECISE_LEGACY_DIAGNOSTIC in diagnostics
    evidence_present = "contact_release_evidence_mode" in diagnostics
    if role == BASELINE_ROLE:
        if legacy_present:
            if diagnostics[PRECISE_LEGACY_DIAGNOSTIC] != "0.000000":
                raise ValueError(
                    f"{label} legacy clearance diagnostic is not exact zero"
                )
            audit["baseline_legacy_zero_observations"] += 1
            diagnostics.pop(PRECISE_LEGACY_DIAGNOSTIC)
        if is_contact_release_violation and not legacy_present:
            raise ValueError(
                f"{label} baseline violation omitted the exact legacy zero"
            )
        if evidence_present:
            raise ValueError(
                f"{label} baseline unexpectedly serialized release evidence mode"
            )
        if is_contact_release_violation:
            diagnostics["contact_release_evidence_mode"] = (
                "signed_penetration"
            )
            audit["baseline_implied_signed_mode_observations"] += 1
    elif role == CURRENT_ROLE:
        if legacy_present:
            raise ValueError(
                f"{label} current result retained the removed legacy diagnostic"
            )
        if is_contact_release_violation:
            if diagnostics.get("contact_release_evidence_mode") != (
                "signed_penetration"
            ):
                raise ValueError(
                    f"{label} current violation omitted exact signed mode"
                )
            audit["current_explicit_signed_mode_observations"] += 1
        elif evidence_present:
            raise ValueError(
                f"{label} current result serialized evidence mode without a violation"
            )
    else:
        raise ValueError(f"{label} has unknown source role {role}")
    return diagnostics


def _precise_pairs(value: Any, label: str) -> list[tuple[str, str]]:
    if not isinstance(value, list):
        raise ValueError(f"{label} must be an array")
    pairs = [canonical_pair(item, f"{label}[{index}]") for index, item in enumerate(value)]
    if pairs != sorted(set(pairs)):
        raise ValueError(f"{label} must be sorted and duplicate-free")
    return pairs


def _precise_classified_pairs(
    value: Any, label: str
) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        raise ValueError(f"{label} must be an array")
    result: list[dict[str, Any]] = []
    observed_pairs: list[tuple[str, str]] = []
    for index, item in enumerate(value):
        classified = exact_object(
            item,
            ("pair", "classification_code", "is_first_failure"),
            f"{label}[{index}]",
        )
        pair = canonical_pair(classified["pair"], f"{label}[{index}].pair")
        code = classified["classification_code"]
        first = classified["is_first_failure"]
        if not isinstance(code, int) or isinstance(code, bool) or code < 0:
            raise ValueError(f"{label}[{index}] classification is invalid")
        if not isinstance(first, bool):
            raise ValueError(f"{label}[{index}] first-failure marker is invalid")
        observed_pairs.append(pair)
        result.append(
            {
                "pair": list(pair),
                "classification_code": code,
                "is_first_failure": first,
            }
        )
    if observed_pairs != sorted(set(observed_pairs)):
        raise ValueError(f"{label} must be sorted and duplicate-free")
    return result


def _precise_normalize_classification_migration(
    classified: list[dict[str, Any]],
    role: str,
    case_id: str,
    waypoint_index: int,
    audit: dict[str, int],
    label: str,
) -> list[dict[str, Any]]:
    if not (
        case_id == "colliding_configured_metric_missing"
        and waypoint_index == 1
    ):
        return classified
    expected_raw_code = 3 if role == BASELINE_ROLE else 8
    if classified != [
        {
            "pair": list(PRECISE_CONFIGURED_PAIR),
            "classification_code": expected_raw_code,
            "is_first_failure": True,
        }
    ]:
        raise ValueError(f"{label} missing-metric classification migration is not exact")
    normalized = [dict(classified[0])]
    normalized[0]["classification_code"] = 8
    audit[
        "baseline_missing_metric_code3_observations"
        if role == BASELINE_ROLE
        else "current_missing_metric_code8_observations"
    ] += 1
    return normalized


def _precise_validate_first_failure(
    decision: dict[str, Any],
    waypoint_records: list[dict[str, Any]],
    label: str,
    *,
    require_semantic_marker: bool,
    expected_pair: tuple[str, str] | None = None,
) -> None:
    marked = [
        (record["waypoint_index"], tuple(classified["pair"]))
        for record in waypoint_records
        for classified in record["classified_pairs"]
        if classified["is_first_failure"]
    ]
    if decision["accepted"]:
        if marked or decision["first_failing_waypoint"] is not None:
            raise ValueError(f"{label} accepted output has a first failure")
        return
    if require_semantic_marker:
        expected_waypoint = decision["first_failing_waypoint"]
        at_decision = [item for item in marked if item[0] == expected_waypoint]
        if (
            len(at_decision) != 1
            or any(item[0] < expected_waypoint for item in marked)
            or any(
                sum(
                    1
                    for item in marked
                    if item[0] == record["waypoint_index"]
                )
                > 1
                for record in waypoint_records
            )
        ):
            raise ValueError(
                f"{label} must mark exactly one pair at the decision first failure"
            )
        if expected_pair is not None and at_decision[0][1] != expected_pair:
            raise ValueError(f"{label} marked the wrong first-failing pair")
    elif len(marked) > 1 or (
        marked and marked[0][0] != decision["first_failing_waypoint"]
    ):
        raise ValueError(f"{label} has inconsistent first-failure markers")


def _precise_expected_query_id(
    waypoint_index: int, waypoint_count: int, worker_count: int
) -> str:
    base, remainder = divmod(waypoint_count, worker_count)
    begin = 0
    for worker in range(worker_count):
        size = base + (1 if worker < remainder else 0)
        end = begin + size
        if begin <= waypoint_index < end:
            return f"validation_coordinator_w{waypoint_index - begin}"
        begin = end
    raise ValueError("waypoint index is outside deterministic partition")


def _precise_normalize_compact_bundle(
    value: Any,
    role: str,
    audit: dict[str, int],
    label: str,
    *,
    expected_record_count: int | None = None,
    semantic_records: bool = True,
    expected_samples: list[dict[str, Any]] | None = None,
    expected_exceptional_volatile_key: str | None = None,
    worker_count: int | None = None,
) -> dict[str, Any]:
    bundle = exact_object(
        value,
        (
            "decision",
            "cancelled",
            "cancellation_reason",
            "cancelled_at_waypoint_index",
            "borrowed_world_revision",
            "final_world_revision",
            "scene_changed_during_validation",
            "churn_window_exceeded",
            "pose_source_retries_used",
            "waypoint_record_count",
            "structural_transcript_fingerprint_fnv1a64",
            "waypoint_records",
        ),
        label,
    )
    decision = _precise_decision(bundle["decision"], f"{label}.decision")
    borrowed_world_revision = _precise_revision(
        bundle["borrowed_world_revision"],
        f"{label}.borrowed_world_revision",
        nullable=False,
    )
    final_world_revision = _precise_revision(
        bundle["final_world_revision"], f"{label}.final_world_revision"
    )
    if not all(
        isinstance(bundle[field], bool)
        for field in (
            "cancelled",
            "scene_changed_during_validation",
            "churn_window_exceeded",
        )
    ):
        raise ValueError(f"{label} bundle booleans are invalid")
    if bundle["cancellation_reason"] is not None and not isinstance(
        bundle["cancellation_reason"], str
    ):
        raise ValueError(f"{label}.cancellation_reason is invalid")
    cancelled_at = bundle["cancelled_at_waypoint_index"]
    if cancelled_at is not None and (
        not isinstance(cancelled_at, int)
        or isinstance(cancelled_at, bool)
        or cancelled_at < 0
    ):
        raise ValueError(f"{label}.cancelled_at_waypoint_index is invalid")
    if (
        not isinstance(bundle["pose_source_retries_used"], int)
        or isinstance(bundle["pose_source_retries_used"], bool)
        or bundle["pose_source_retries_used"] < 0
        or not isinstance(bundle["waypoint_record_count"], int)
        or isinstance(bundle["waypoint_record_count"], bool)
        or bundle["waypoint_record_count"] < 0
        or not fingerprint_string(
            bundle["structural_transcript_fingerprint_fnv1a64"]
        )
    ):
        raise ValueError(f"{label} bundle counters/fingerprint are invalid")
    records_value = exact_list(
        bundle["waypoint_records"], bundle["waypoint_record_count"],
        f"{label}.waypoint_records"
    )
    if expected_record_count is not None and len(records_value) != expected_record_count:
        raise ValueError(f"{label} has the wrong waypoint record count")
    records: list[dict[str, Any]] = []
    omitted_exceptional_keys: list[str] = []
    for index, item in enumerate(records_value):
        record = exact_object(
            item,
            (
                "waypoint_index",
                "time_from_start_ns",
                "query_id",
                "collision_world_revision",
                "policy_revision",
                "policy_revision_used",
                "surface_code",
                "state_time_ns",
                "engine_id",
                "engine_version",
                "sampling_mode_code",
                "contact_body_a",
                "contact_body_b",
                "query_duration_ns_omitted_as_volatile",
                "produced_decision_change",
                "engine_status_code",
                "decision_code",
                "in_collision",
                "normalized_pairs",
                "classified_pairs",
                "raw_distance_scope",
                "raw_distance_request_mode_field_present",
                "raw_distance_request_mode_code",
                "raw_evaluated_distance_pair_count_field_present",
                "raw_evaluated_distance_pair_count",
                "raw_pair_clearances",
                "raw_min_clearance_m",
                "raw_min_clearance_pair",
                "nonvolatile_raw_diagnostics",
                "omitted_volatile_diagnostic_count",
                "omitted_exceptional_volatile_diagnostic_keys",
            ),
            f"{label}.waypoint_records[{index}]",
        )
        if (
            not isinstance(record["waypoint_index"], int)
            or isinstance(record["waypoint_index"], bool)
            or record["waypoint_index"] < 0
            or record["time_from_start_ns"] is not None
            and (
                not isinstance(record["time_from_start_ns"], int)
                or isinstance(record["time_from_start_ns"], bool)
                or record["time_from_start_ns"] < 0
            )
            or not isinstance(record["produced_decision_change"], bool)
            or not isinstance(record["engine_status_code"], int)
            or isinstance(record["engine_status_code"], bool)
            or not isinstance(record["decision_code"], int)
            or isinstance(record["decision_code"], bool)
            or not isinstance(record["in_collision"], bool)
        ):
            raise ValueError(f"{label}.waypoint_records[{index}] is malformed")
        pairs = _precise_pairs(
            record["normalized_pairs"],
            f"{label}.waypoint_records[{index}].normalized_pairs",
        )
        classified = _precise_classified_pairs(
            record["classified_pairs"],
            f"{label}.waypoint_records[{index}].classified_pairs",
        )
        diagnostics = _precise_normalize_diagnostics(
            record["nonvolatile_raw_diagnostics"],
            role,
            audit,
            f"{label}.waypoint_records[{index}].diagnostics",
        )
        omitted = record["omitted_volatile_diagnostic_count"]
        if omitted != 0:
            raise ValueError(
                f"{label}.waypoint_records[{index}] concealed a synthetic diagnostic"
            )
        exceptional_keys = record[
            "omitted_exceptional_volatile_diagnostic_keys"
        ]
        if not (
            isinstance(exceptional_keys, list)
            and all(isinstance(key, str) and key for key in exceptional_keys)
            and exceptional_keys == sorted(set(exceptional_keys))
            and set(exceptional_keys) <= {"elapsed_ns"}
        ):
            raise ValueError(
                f"{label}.waypoint_records[{index}] exceptional volatility is invalid"
            )
        omitted_exceptional_keys.extend(exceptional_keys)
        stable_fields = _precise_stable_result_fields(
            record, f"{label}.waypoint_records[{index}]"
        )
        waypoint = record["waypoint_index"]
        if (
            expected_samples is None
            or waypoint >= len(expected_samples)
            or worker_count is None
        ):
            raise ValueError(f"{label} lacks an exact input sample for waypoint {waypoint}")
        expected_sample = expected_samples[waypoint]
        expected_query_id = (
            _precise_expected_query_id(
                waypoint, len(expected_samples), worker_count
            )
            if record["engine_status_code"] == 0
            else None
        )
        if stable_fields["query_id"] != expected_query_id:
            raise ValueError(
                f"{label}.waypoint_records[{index}] query id is not the exact chunk-local id"
            )
        if record["engine_status_code"] == 0 and (
            stable_fields["engine_id"] != "step18_precise_reference_engine"
            or stable_fields["engine_version"] != "1"
        ):
            raise ValueError(
                f"{label}.waypoint_records[{index}] engine identity drifted"
            )
        if pairs != expected_sample["colliding_pairs"] and record[
            "engine_status_code"
        ] == 0:
            raise ValueError(f"{label} physical pair output differs from its input")
        projected = _precise_normalize_raw_distance(
            record,
            role,
            pairs,
            expected_sample["configured_signed_clearance_m"],
            f"{label}.waypoint_records[{index}]",
            engine_status_code=record["engine_status_code"],
        )
        records.append(
            {
                "waypoint_index": record["waypoint_index"],
                "time_from_start_ns": record["time_from_start_ns"],
                **stable_fields,
                "produced_decision_change": record[
                    "produced_decision_change"
                ],
                "engine_status_code": record["engine_status_code"],
                "decision_code": record["decision_code"],
                "in_collision": record["in_collision"],
                "normalized_pairs": [list(pair) for pair in pairs],
                "classified_pairs": classified,
                "projected_configured_colliding_clearances": projected,
                "diagnostics": diagnostics,
            }
        )
    if semantic_records:
        if [record["waypoint_index"] for record in records] != list(
            range(len(records))
        ):
            raise ValueError(f"{label} waypoint order is not exact")
        if any(record["engine_status_code"] != 0 for record in records):
            raise ValueError(f"{label} semantic fixture has a non-OK engine status")
    expected_omitted_exceptional = (
        []
        if expected_exceptional_volatile_key is None
        else [expected_exceptional_volatile_key]
    )
    if omitted_exceptional_keys != expected_omitted_exceptional:
        raise ValueError(f"{label} exceptional volatile-key audit is not exact")
    _precise_validate_first_failure(
        decision,
        records,
        label,
        require_semantic_marker=semantic_records and not decision["accepted"],
    )
    return {
        "decision": decision,
        "cancelled": bundle["cancelled"],
        "cancellation_reason": bundle["cancellation_reason"],
        "cancelled_at_waypoint_index": cancelled_at,
        "borrowed_world_revision": borrowed_world_revision,
        "final_world_revision": final_world_revision,
        "scene_changed_during_validation": bundle[
            "scene_changed_during_validation"
        ],
        "churn_window_exceeded": bundle["churn_window_exceeded"],
        "pose_source_retries_used": bundle["pose_source_retries_used"],
        "waypoint_records": records,
    }


def _precise_expected_raw_clearances(
    role: str,
    colliding_pairs: list[tuple[str, str]],
    configured_clearance: float | None,
) -> list[tuple[tuple[str, str], float]]:
    if role == CURRENT_ROLE:
        if (
            PRECISE_CONFIGURED_PAIR in colliding_pairs
            and configured_clearance is not None
        ):
            return [(PRECISE_CONFIGURED_PAIR, configured_clearance)]
        return []
    if role != BASELINE_ROLE:
        raise ValueError(f"unknown precise-reference source role {role}")
    result: list[tuple[tuple[str, str], float]] = []
    if configured_clearance is not None:
        result.append((PRECISE_CONFIGURED_PAIR, configured_clearance))
    elif PRECISE_CONFIGURED_PAIR not in colliding_pairs:
        result.append((PRECISE_CONFIGURED_PAIR, 0.010))
    if PRECISE_UNCONFIGURED_PAIR in colliding_pairs:
        result.append((PRECISE_UNCONFIGURED_PAIR, -0.002))
    result.append((PRECISE_FAR_PAIR, 1.0))
    return sorted(result)


def _precise_normalize_raw_distance(
    record: dict[str, Any],
    role: str,
    colliding_pairs: list[tuple[str, str]],
    configured_clearance: float | None,
    label: str,
    *,
    engine_status_code: int,
) -> list[dict[str, Any]]:
    expected_scope = (
        "all_active_pairs"
        if role == BASELINE_ROLE
        else "selected_colliding_pairs"
    )
    if record["raw_distance_scope"] != expected_scope:
        raise ValueError(f"{label} raw distance scope is invalid")
    if role == BASELINE_ROLE:
        presence_ok = (
            record["raw_distance_request_mode_field_present"] is False
            and record["raw_distance_request_mode_code"] is None
            and record[
                "raw_evaluated_distance_pair_count_field_present"
            ]
            is False
        )
    else:
        presence_ok = (
            record["raw_distance_request_mode_field_present"] is True
            and record["raw_distance_request_mode_code"]
            == (1 if engine_status_code == 0 else 0)
            and record[
                "raw_evaluated_distance_pair_count_field_present"
            ]
            is True
        )
    if not presence_ok:
        raise ValueError(f"{label} raw distance DTO presence is invalid")
    raw_clearances_value = record["raw_pair_clearances"]
    if not isinstance(raw_clearances_value, list):
        raise ValueError(f"{label} raw clearances must be an array")
    raw_clearances: list[tuple[tuple[str, str], float]] = []
    for clearance_index, clearance_value in enumerate(raw_clearances_value):
        clearance = exact_object(
            clearance_value,
            ("pair", "signed_clearance_m"),
            f"{label}.raw_pair_clearances[{clearance_index}]",
        )
        pair = canonical_pair(
            clearance["pair"],
            f"{label}.raw_pair_clearances[{clearance_index}].pair",
        )
        signed = clearance["signed_clearance_m"]
        if not (
            isinstance(signed, (int, float))
            and not isinstance(signed, bool)
            and math.isfinite(signed)
        ):
            raise ValueError(f"{label} raw clearance is not finite")
        raw_clearances.append((pair, float(signed)))
    if raw_clearances != sorted(raw_clearances) or len(
        {pair for pair, _ in raw_clearances}
    ) != len(raw_clearances):
        raise ValueError(f"{label} raw clearances are not canonical")
    expected_raw = (
        _precise_expected_raw_clearances(
            role, colliding_pairs, configured_clearance
        )
        if engine_status_code == 0
        else []
    )
    count = record["raw_evaluated_distance_pair_count"]
    if (
        not isinstance(count, int)
        or isinstance(count, bool)
        or count < 0
        or raw_clearances != expected_raw
        or count != len(expected_raw)
    ):
        raise ValueError(f"{label} raw distance representation is not exact")
    if expected_raw:
        expected_min_pair, expected_min = min(
            expected_raw, key=lambda item: item[1]
        )
        if (
            record["raw_min_clearance_m"] != expected_min
            or canonical_pair(
                record["raw_min_clearance_pair"],
                f"{label}.raw_min_clearance_pair",
            )
            != expected_min_pair
        ):
            raise ValueError(f"{label} raw minimum clearance is invalid")
    elif (
        record["raw_min_clearance_m"] is not None
        or record["raw_min_clearance_pair"] is not None
    ):
        raise ValueError(f"{label} empty raw clearance result has a minimum")
    return [
        {"pair": list(pair), "signed_clearance_m": signed}
        for pair, signed in raw_clearances
        if pair == PRECISE_CONFIGURED_PAIR and pair in colliding_pairs
    ]


def _precise_normalize_small_case(
    value: Any,
    role: str,
    worker_count: int,
    audit: dict[str, int],
    label: str,
) -> dict[str, Any]:
    case = exact_object(
        value,
        ("case_id", "input_fingerprint_fnv1a64", "input", "execution", "output"),
        label,
    )
    case_id = case["case_id"]
    expected_cases = precise_reference_case_inputs()
    if case_id not in expected_cases or not fingerprint_string(
        case["input_fingerprint_fnv1a64"]
    ):
        raise ValueError(f"{label} case identity/fingerprint is invalid")
    input_value = exact_object(
        case["input"], ("policy", "configured_pair", "samples"),
        f"{label}.input"
    )
    policy = exact_object(
        input_value["policy"],
        (
            "mode",
            "release_evidence_mode",
            "raw_release_evidence_mode_field_present",
            "raw_release_evidence_mode",
            "enforce_max_start_penetration",
            "max_start_penetration_m",
            "release_tolerance_m",
            "min_release_progress_m",
            "require_separation_by_end",
            "require_strictly_decreasing_penetration",
            "allow_recontact",
        ),
        f"{label}.input.policy",
    )
    expected_policy = expected_cases[case_id]["policy"]
    normalized_policy = {
        key: policy[key]
        for key in expected_policy
    }
    if normalized_policy != expected_policy:
        raise ValueError(f"{label} normalized policy input is not exact")
    if role == BASELINE_ROLE:
        raw_mode_ok = (
            policy["raw_release_evidence_mode_field_present"] is False
            and policy["raw_release_evidence_mode"] is None
        )
        audit["baseline_raw_mode_absent_profiles"] += 1
    else:
        raw_mode_ok = (
            policy["raw_release_evidence_mode_field_present"] is True
            and policy["raw_release_evidence_mode"] == "signed_penetration"
        )
        audit["current_raw_mode_present_profiles"] += 1
    if not raw_mode_ok:
        raise ValueError(f"{label} raw release-evidence field shape is invalid")
    configured_pair = canonical_pair(
        input_value["configured_pair"], f"{label}.input.configured_pair"
    )
    if configured_pair != PRECISE_CONFIGURED_PAIR:
        raise ValueError(f"{label} configured pair is not exact")
    samples_value = exact_list(input_value["samples"], 5, f"{label}.input.samples")
    normalized_samples: list[dict[str, Any]] = []
    expected_samples = expected_cases[case_id]["samples"]
    for index, item in enumerate(samples_value):
        sample = exact_object(
            item,
            (
                "waypoint_index",
                "time_from_start_ns",
                "colliding_pairs",
                "configured_signed_clearance_m",
            ),
            f"{label}.input.samples[{index}]",
        )
        pairs = _precise_pairs(
            sample["colliding_pairs"],
            f"{label}.input.samples[{index}].colliding_pairs",
        )
        clearance = sample["configured_signed_clearance_m"]
        if clearance is not None and not (
            isinstance(clearance, (int, float))
            and not isinstance(clearance, bool)
            and math.isfinite(clearance)
        ):
            raise ValueError(f"{label}.input.samples[{index}] clearance is invalid")
        expected_sample = expected_samples[index]
        if (
            sample["waypoint_index"] != index
            or sample["time_from_start_ns"] != index * 10_000_000
            or pairs != expected_sample["colliding_pairs"]
            or clearance != expected_sample["configured_signed_clearance_m"]
        ):
            raise ValueError(f"{label}.input.samples[{index}] is not exact")
        normalized_samples.append(
            {
                "waypoint_index": index,
                "time_from_start_ns": index * 10_000_000,
                "colliding_pairs": [list(pair) for pair in pairs],
                "configured_signed_clearance_m": clearance,
            }
        )
    execution = exact_object(
        case["execution"],
        ("worker_count_effective", "workers_used", "fallback_reason"),
        f"{label}.execution",
    )
    expected_fallback = "worker_count_one" if worker_count == 1 else ""
    if execution != {
        "worker_count_effective": worker_count,
        "workers_used": worker_count,
        "fallback_reason": expected_fallback,
    }:
        raise ValueError(f"{label} execution profile is not exact")
    output = exact_object(
        case["output"],
        (
            "decision",
            "borrowed_world_revision",
            "final_world_revision",
            "pose_source_retries_used",
            "scene_changed_during_validation",
            "churn_window_exceeded",
            "cancelled",
            "cancellation_reason",
            "cancelled_at_waypoint_index",
            "waypoint_records",
        ),
        f"{label}.output",
    )
    decision = _precise_decision(output["decision"], f"{label}.output.decision")
    borrowed_world_revision = _precise_revision(
        output["borrowed_world_revision"],
        f"{label}.output.borrowed_world_revision",
        nullable=False,
    )
    final_world_revision = _precise_revision(
        output["final_world_revision"], f"{label}.output.final_world_revision"
    )
    if not (
        output["pose_source_retries_used"] == 0
        and output["scene_changed_during_validation"] is False
        and output["churn_window_exceeded"] is False
        and output["cancelled"] is False
        and output["cancellation_reason"] is None
        and output["cancelled_at_waypoint_index"] is None
    ):
        raise ValueError(f"{label} small-case bundle metadata is not exact")
    decision_tuple = (
        decision["accepted"],
        decision["action_code"],
        decision["severity_code"],
        decision["first_failing_waypoint"],
        decision["first_failing_time_ns"],
    )
    if decision_tuple != PRECISE_REFERENCE_EXPECTED_DECISIONS[case_id]:
        raise ValueError(f"{label} decision is not the approved semantic result")
    waypoint_values = exact_list(
        output["waypoint_records"], 5, f"{label}.output.waypoint_records"
    )
    normalized_records: list[dict[str, Any]] = []
    for index, item in enumerate(waypoint_values):
        record = exact_object(
            item,
            (
                "waypoint_index",
                "time_from_start_ns",
                "query_id",
                "collision_world_revision",
                "policy_revision",
                "policy_revision_used",
                "surface_code",
                "state_time_ns",
                "engine_id",
                "engine_version",
                "sampling_mode_code",
                "contact_body_a",
                "contact_body_b",
                "query_duration_ns_omitted_as_volatile",
                "produced_decision_change",
                "engine_status_code",
                "decision_code",
                "in_collision",
                "normalized_pairs",
                "classified_pairs",
                "raw_distance_scope",
                "raw_distance_request_mode_field_present",
                "raw_distance_request_mode_code",
                "raw_evaluated_distance_pair_count_field_present",
                "raw_evaluated_distance_pair_count",
                "raw_pair_clearances",
                "raw_min_clearance_m",
                "raw_min_clearance_pair",
                "nonvolatile_raw_diagnostics",
                "omitted_volatile_diagnostic_count",
            ),
            f"{label}.output.waypoint_records[{index}]",
        )
        sample = normalized_samples[index]
        pairs = _precise_pairs(
            record["normalized_pairs"],
            f"{label}.output.waypoint_records[{index}].normalized_pairs",
        )
        if (
            record["waypoint_index"] != index
            or record["time_from_start_ns"] != index * 10_000_000
            or not isinstance(record["produced_decision_change"], bool)
            or record["engine_status_code"] != 0
            or not isinstance(record["decision_code"], int)
            or isinstance(record["decision_code"], bool)
            or record["in_collision"] is not bool(pairs)
            or pairs != [tuple(pair) for pair in sample["colliding_pairs"]]
        ):
            raise ValueError(f"{label}.output.waypoint_records[{index}] physical result is invalid")
        classified = _precise_classified_pairs(
            record["classified_pairs"],
            f"{label}.output.waypoint_records[{index}].classified_pairs",
        )
        classified = _precise_normalize_classification_migration(
            classified,
            role,
            case_id,
            index,
            audit,
            label,
        )
        stable_fields = _precise_stable_result_fields(
            record, f"{label}.output.waypoint_records[{index}]"
        )
        if stable_fields["query_id"] != _precise_expected_query_id(
            index, 5, worker_count
        ):
            raise ValueError(
                f"{label}.output.waypoint_records[{index}] query id is not the exact chunk-local id"
            )
        if (
            stable_fields["engine_id"] != "step18_precise_reference_engine"
            or stable_fields["engine_version"] != "1"
        ):
            raise ValueError(
                f"{label}.output.waypoint_records[{index}] engine identity drifted"
            )
        projected = _precise_normalize_raw_distance(
            record,
            role,
            pairs,
            sample["configured_signed_clearance_m"],
            f"{label}.output.waypoint_records[{index}]",
            engine_status_code=record["engine_status_code"],
        )
        omitted = record["omitted_volatile_diagnostic_count"]
        if omitted != 0:
            raise ValueError(
                f"{label}.output.waypoint_records[{index}] concealed a synthetic diagnostic"
            )
        diagnostics = _precise_normalize_diagnostics(
            record["nonvolatile_raw_diagnostics"],
            role,
            audit,
            f"{label}.output.waypoint_records[{index}].diagnostics",
        )
        normalized_records.append(
            {
                "waypoint_index": index,
                "time_from_start_ns": record["time_from_start_ns"],
                **stable_fields,
                "produced_decision_change": record[
                    "produced_decision_change"
                ],
                "engine_status_code": record["engine_status_code"],
                "decision_code": record["decision_code"],
                "in_collision": record["in_collision"],
                "normalized_pairs": [list(pair) for pair in pairs],
                "classified_pairs": classified,
                "projected_configured_colliding_clearances": projected,
                "diagnostics": diagnostics,
            }
        )
    expected_failure_pair = (
        PRECISE_UNCONFIGURED_PAIR
        if case_id == "equal_count_unconfigured_pair_substitution"
        else PRECISE_CONFIGURED_PAIR
    )
    _precise_validate_first_failure(
        decision,
        normalized_records,
        label,
        require_semantic_marker=not decision["accepted"],
        expected_pair=(None if decision["accepted"] else expected_failure_pair),
    )
    return {
        "case_id": case_id,
        "input": {
            "policy": normalized_policy,
            "configured_pair": list(configured_pair),
            "samples": normalized_samples,
        },
        "output": {
            "decision": decision,
            "borrowed_world_revision": borrowed_world_revision,
            "final_world_revision": final_world_revision,
            "pose_source_retries_used": output["pose_source_retries_used"],
            "scene_changed_during_validation": output[
                "scene_changed_during_validation"
            ],
            "churn_window_exceeded": output["churn_window_exceeded"],
            "cancelled": output["cancelled"],
            "cancellation_reason": output["cancellation_reason"],
            "cancelled_at_waypoint_index": output[
                "cancelled_at_waypoint_index"
            ],
            "waypoint_records": normalized_records,
        },
    }


def _precise_partition_samples(kind_indices: list[int]) -> list[dict[str, Any]]:
    samples: list[dict[str, Any]] = []
    for kind in kind_indices:
        if kind == 0:
            samples.append(_precise_sample(False, None))
        elif kind == 1:
            samples.append(_precise_sample(True, -0.040))
        elif kind == 2:
            samples.append(_precise_sample(True, -0.015))
        elif kind == 3:
            samples.append(_precise_sample(False, 0.002))
        else:
            raise ValueError(f"invalid precise-reference sample kind {kind}")
    return samples


def _precise_completion_samples() -> list[dict[str, Any]]:
    return [
        _precise_sample(clearance < 0.0, clearance)
        for clearance in PRECISE_REFERENCE_COMPLETION_CLEARANCES
    ]


def _precise_normalization_contract() -> dict[str, Any]:
    return {
        "id": PRECISE_REFERENCE_NORMALIZATION_ID,
        "allowed_semantic_delta": "closed_step18_precise_reference_migration",
        "clearance_projection": "configured_intersection_colliding_pairs",
        "baseline_distance_scope": "all_active_pairs",
        "current_distance_scope": "selected_colliding_pairs",
        "normalized_release_evidence_mode": "signed_penetration",
        "baseline_zero_only_legacy_diagnostic_count": 1,
        "legacy_baseline_diagnostic": {
            "field": PRECISE_LEGACY_DIAGNOSTIC,
            "required_baseline_value": 0,
            "required_baseline_serialized_value": "0.000000",
            "baseline_presence": "required_on_violation_records",
            "current_presence": "forbidden",
        },
        "raw_release_evidence_mode_field": {
            "baseline_presence": False,
            "baseline_value": None,
            "current_presence": True,
            "current_value": "signed_penetration",
        },
        "missing_metric_classification_migration": {
            "case_id": "colliding_configured_metric_missing",
            "waypoint_index": 1,
            "pair": {
                "body_a": PRECISE_CONFIGURED_PAIR[0],
                "body_b": PRECISE_CONFIGURED_PAIR[1],
            },
            "baseline_classification_code": 3,
            "current_classification_code": 8,
            "normalized_classification_code": 8,
            "reason": "missing_contact_release_clearance",
            "accepted": False,
            "action_code": 3,
            "severity_code": 2,
            "first_failing_waypoint": 1,
            "first_failing_time_ns": 10_000_000,
        },
        "volatile_result_fields": ["query_duration_ns"],
        "global_volatile_diagnostic_keys": list(
            PRECISE_GLOBAL_VOLATILE_DIAGNOSTICS
        ),
        "exceptional_volatile_diagnostic_keys": {
            "expired_absolute_deadline": ["elapsed_ns"]
        },
    }


def _precise_orchestration_projection_contract() -> dict[str, Any]:
    return {
        "id": "step18_cross_worker_orchestration_projection.v1",
        "scope": "cross_worker_count_only",
        "excluded_result_fields": ["query_id"],
        "matched_source_profile_parity": "full_transcript_including_query_id",
    }


def _precise_orchestration_projection(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: _precise_orchestration_projection(item)
            for key, item in value.items()
            if key != "query_id"
        }
    if isinstance(value, list):
        return [_precise_orchestration_projection(item) for item in value]
    return value


def _precise_expected_exception(case_id: str) -> dict[str, Any]:
    values = {
        "pre_cancelled": {
            "accepted": False,
            "action_code": 3,
            "severity_code": 2,
            "first_failing_waypoint": 0,
            "first_failing_time_ns": 0,
            "engine_status_code": None,
            "fallback_reason": "",
            "reason": "operator_stop",
        },
        "expired_absolute_deadline": {
            "accepted": False,
            "action_code": 3,
            "severity_code": 2,
            "first_failing_waypoint": 0,
            "first_failing_time_ns": None,
            "engine_status_code": 1,
            "fallback_reason": "validation_wall_timeout",
            "reason": "collision_validation_wall_timeout",
        },
        "worker_exception": {
            "accepted": False,
            "action_code": 3,
            "severity_code": 2,
            "first_failing_waypoint": 4,
            "first_failing_time_ns": 40_000_000,
            "engine_status_code": 5,
            "fallback_reason": "",
            "reason": "collision_validation_worker_exception",
        },
    }
    return values[case_id]


def validate_precise_reference(
    record: dict[str, Any], checks: Checks
) -> tuple[str, dict[str, Any]] | None:
    label = record_label(record)
    identity = record.get("identity", {})
    provenance = record.get("provenance", {})
    payload = record.get("payload", {})
    role = identity.get("source_role")
    metadata_ok = (
        isinstance(payload, dict)
        and set(payload) == PRECISE_REFERENCE_PAYLOAD_KEYS
        and role in {BASELINE_ROLE, CURRENT_ROLE}
        and payload.get("schema_version") == PRECISE_REFERENCE_SCHEMA
        and payload.get("record_type") == "precise_reference_parity"
        and payload.get("passed") is True
        and payload.get("timing_gate_excluded") is True
        and payload.get("measurement_profile_usage")
        == "provenance_only_no_timing_samples"
        and payload.get("source_role") == role
        and payload.get("image_id") == identity.get("image_id")
        and payload.get("binary_sha256") == identity.get("binary_sha256")
        and payload.get("run_id") == identity.get("run_id")
        and payload.get("process_instance_id")
        == identity.get("process_instance_id")
        and payload.get("requested_cpu_affinity")
        == identity.get("requested_cpu_affinity")
        and payload.get("effective_cpu_affinity")
        == identity.get("requested_cpu_affinity")
        and payload.get("warmup_runs") == WARMUP_RUNS
        and payload.get("measured_runs") == MEASURED_RUNS
        and payload.get("build_type") == "Release"
        and payload.get("compiler_flags") == "-Wno-error=maybe-uninitialized"
        and payload.get("clean_checkout_manifest_sha256")
        == provenance.get("clean_checkout_manifest_sha256")
        and payload.get("qualification_patch_sha256")
        == provenance.get("qualification_patch_sha256")
        and sha256_string(payload.get("corpus_sha256"))
        and isinstance(payload.get("scheduler_policy"), str)
        and isinstance(payload.get("scheduler_priority"), int)
        and isinstance(payload.get("process_nice"), int)
        and isinstance(payload.get("kernel_id"), str)
        and isinstance(payload.get("cpu_governor_set"), str)
        and isinstance(payload.get("cpu_frequency_driver_set"), str)
        and isinstance(payload.get("compiler"), str)
    )
    command = provenance.get("command_argv", [])
    runner_command = provenance.get("runner_command_argv", [])
    expected_binary_name = (
        "perf_policy_support_contact_baseline"
        if role == BASELINE_ROLE
        else "perf_policy_support_contact_qualification"
    )
    expected_runner_name = (
        "run-step18-validation-nonregression-baseline.sh"
        if role == BASELINE_ROLE
        else "run-step18-proof-cell-qualification.sh"
    )
    metadata_ok = metadata_ok and (
        isinstance(command, list)
        and len(command) == 9
        and command[:3]
        == ["taskset", "-c", identity.get("requested_cpu_affinity")]
        and Path(command[3]).name == expected_binary_name
        and command[4:]
        == [
            "--precise-reference-only",
            "--warmup-runs",
            "5",
            "--measured-runs",
            "50",
        ]
        and isinstance(runner_command, list)
        and len(runner_command) == 3
        and Path(runner_command[0]).name == expected_runner_name
        and runner_command[1:] == ["--cell", "precise-reference-parity"]
    )
    forbidden_timing_fields = {
        "performance",
        "wall_us",
        "process_cpu_us",
        "latency_ns",
        "cpu_time_ns",
        "per_state_latency_ns",
        "per_state_cpu_time_ns",
    }
    metadata_ok = metadata_ok and not (
        forbidden_timing_fields & set(payload)
    )
    checks.require(f"{label}:precise_reference_metadata", metadata_ok)
    if not metadata_ok:
        return None
    audit = {
        "baseline_legacy_zero_observations": 0,
        "baseline_implied_signed_mode_observations": 0,
        "current_explicit_signed_mode_observations": 0,
        "baseline_raw_mode_absent_profiles": 0,
        "current_raw_mode_present_profiles": 0,
        "baseline_missing_metric_code3_observations": 0,
        "current_missing_metric_code8_observations": 0,
        "omitted_global_volatile_diagnostic_values": 0,
    }
    try:
        evidence = exact_object(
            payload.get("reference_evidence"),
            (
                "normalization_contract",
                "orchestration_projection_contract",
                "small_case_profiles",
                "partition_inventory",
                "completion_order_inventory",
                "exceptional_cases",
            ),
            f"{label}.reference_evidence",
        )
        contract = evidence["normalization_contract"]
        if contract != _precise_normalization_contract():
            raise ValueError(f"{label} normalization contract is not exact")
        orchestration_contract = evidence[
            "orchestration_projection_contract"
        ]
        if orchestration_contract != (
            _precise_orchestration_projection_contract()
        ):
            raise ValueError(
                f"{label} orchestration projection contract is not exact"
            )
        small_profiles_value = exact_list(
            evidence["small_case_profiles"], 5,
            f"{label}.small_case_profiles"
        )
        small_profile_sha256: dict[str, str] = {}
        small_orchestration_profile_sha256: dict[str, str] = {}
        small_case_sha256: dict[str, dict[str, str]] = {}
        small_inputs: list[dict[str, Any]] | None = None
        for profile_index, profile_value in enumerate(small_profiles_value):
            workers = PRECISE_REFERENCE_WORKERS[profile_index]
            profile = exact_object(
                profile_value,
                (
                    "schema_version",
                    "execution_surface",
                    "worker_count_requested",
                    "case_count",
                    "normalization_contract",
                    "case_order",
                    "cases",
                ),
                f"{label}.small_case_profiles[{profile_index}]",
            )
            nested_contract = {
                "allowed_semantic_delta": "closed_step18_precise_reference_migration",
                "baseline_distance_scope": "all_active_pairs",
                "current_distance_scope": "selected_colliding_pairs",
                "clearance_projection": "configured_intersection_colliding_pairs",
                "normalized_release_evidence_mode": "signed_penetration",
                "volatile_diagnostic_keys": list(
                    PRECISE_GLOBAL_VOLATILE_DIAGNOSTICS
                ),
            }
            if not (
                profile["schema_version"] == PRECISE_REFERENCE_CORPUS_SCHEMA
                and profile["execution_surface"]
                == "actual_parallel_collision_validation_worker_pool_untimed"
                and profile["worker_count_requested"] == workers
                and profile["case_count"] == len(PRECISE_REFERENCE_CASE_ORDER)
                and profile["normalization_contract"] == nested_contract
                and profile["case_order"] == list(PRECISE_REFERENCE_CASE_ORDER)
            ):
                raise ValueError(f"{label} small profile {workers} is not exact")
            cases_value = exact_list(
                profile["cases"], len(PRECISE_REFERENCE_CASE_ORDER),
                f"{label}.small_case_profiles[{profile_index}].cases"
            )
            normalized_cases = [
                _precise_normalize_small_case(
                    case_value,
                    role,
                    workers,
                    audit,
                    f"{label}.small[{workers}].{case_id}",
                )
                for case_id, case_value in zip(
                    PRECISE_REFERENCE_CASE_ORDER, cases_value, strict=True
                )
            ]
            if [case["case_id"] for case in normalized_cases] != list(
                PRECISE_REFERENCE_CASE_ORDER
            ):
                raise ValueError(f"{label} small case order drifted")
            profile_inputs = [case["input"] for case in normalized_cases]
            if small_inputs is None:
                small_inputs = profile_inputs
            elif profile_inputs != small_inputs:
                raise ValueError(f"{label} small input corpus differs by worker count")
            outputs = [
                {"case_id": case["case_id"], "output": case["output"]}
                for case in normalized_cases
            ]
            small_profile_sha256[str(workers)] = canonical_sha256(outputs)
            small_orchestration_profile_sha256[str(workers)] = canonical_sha256(
                _precise_orchestration_projection(outputs)
            )
            small_case_sha256[str(workers)] = {
                case["case_id"]: canonical_sha256(case["output"])
                for case in normalized_cases
            }
        if len(set(small_orchestration_profile_sha256.values())) != 1:
            raise ValueError(
                f"{label} small orchestration projection differs by worker count"
            )

        partition = exact_object(
            evidence["partition_inventory"],
            (
                "sample_kind_count",
                "waypoint_count",
                "encoding",
                "sample_kinds",
                "patterns_per_worker",
                "serial_worker_count",
                "partition_worker_counts",
                "profiles",
            ),
            f"{label}.partition_inventory",
        )
        expected_sample_kinds = [
            {"kind": "absent", "signed_clearance_m": None},
            {"kind": "colliding", "signed_clearance_m": -0.04},
            {"kind": "colliding", "signed_clearance_m": -0.015},
            {"kind": "separated", "signed_clearance_m": 0.002},
        ]
        if not (
            partition["sample_kind_count"] == 4
            and partition["waypoint_count"] == 5
            and partition["encoding"] == "little_endian_base4"
            and partition["sample_kinds"] == expected_sample_kinds
            and partition["patterns_per_worker"] == 1024
            and partition["serial_worker_count"] == 1
            and partition["partition_worker_counts"]
            == list(PRECISE_REFERENCE_PARTITION_WORKERS)
        ):
            raise ValueError(f"{label} partition declaration is not exact")
        partition_profiles = exact_list(
            partition["profiles"], 5, f"{label}.partition_inventory.profiles"
        )
        partition_profile_sha256: dict[str, str] = {}
        partition_orchestration_profile_sha256: dict[str, str] = {}
        partition_inputs: list[list[int]] | None = None
        for profile_index, profile_value in enumerate(partition_profiles):
            workers = PRECISE_REFERENCE_WORKERS[profile_index]
            profile = exact_object(
                profile_value, ("worker_count", "outcome_count", "outcomes"),
                f"{label}.partition[{workers}]"
            )
            outcomes_value = exact_list(
                profile["outcomes"], 1024, f"{label}.partition[{workers}].outcomes"
            )
            if profile["worker_count"] != workers or profile["outcome_count"] != 1024:
                raise ValueError(f"{label} partition worker profile is not exact")
            normalized_outcomes: list[dict[str, Any]] = []
            observed_inputs: list[list[int]] = []
            for pattern_index, outcome_value in enumerate(outcomes_value):
                outcome = exact_object(
                    outcome_value,
                    (
                        "pattern_index",
                        "sample_kind_indices",
                        "input_fingerprint_fnv1a64",
                        "transcript",
                    ),
                    f"{label}.partition[{workers}].outcomes[{pattern_index}]",
                )
                encoded = pattern_index
                expected_kinds: list[int] = []
                for _ in range(5):
                    expected_kinds.append(encoded % 4)
                    encoded //= 4
                if not (
                    outcome["pattern_index"] == pattern_index
                    and outcome["sample_kind_indices"] == expected_kinds
                    and fingerprint_string(outcome["input_fingerprint_fnv1a64"])
                ):
                    raise ValueError(f"{label} partition input mapping is not exact")
                samples = _precise_partition_samples(expected_kinds)
                transcript = _precise_normalize_compact_bundle(
                    outcome["transcript"],
                    role,
                    audit,
                    f"{label}.partition[{workers}].outcomes[{pattern_index}]",
                    expected_record_count=5,
                    expected_samples=samples,
                    worker_count=workers,
                )
                observed_inputs.append(expected_kinds)
                normalized_outcomes.append(
                    {"pattern_index": pattern_index, "transcript": transcript}
                )
            if partition_inputs is None:
                partition_inputs = observed_inputs
            elif observed_inputs != partition_inputs:
                raise ValueError(f"{label} partition inputs differ by worker count")
            partition_profile_sha256[str(workers)] = canonical_sha256(
                normalized_outcomes
            )
            partition_orchestration_profile_sha256[str(workers)] = (
                canonical_sha256(
                    _precise_orchestration_projection(normalized_outcomes)
                )
            )
        if len(set(partition_orchestration_profile_sha256.values())) != 1:
            raise ValueError(
                f"{label} exhaustive partition orchestration projection differs"
            )

        completion = exact_object(
            evidence["completion_order_inventory"],
            (
                "worker_count",
                "permutation_count",
                "input_signed_clearances_m",
                "input_fingerprint_fnv1a64",
                "serial_reference",
                "orders",
            ),
            f"{label}.completion_order_inventory",
        )
        completion_samples = _precise_completion_samples()
        if not (
            completion["worker_count"] == 3
            and completion["permutation_count"] == 6
            and completion["input_signed_clearances_m"]
            == list(PRECISE_REFERENCE_COMPLETION_CLEARANCES)
            and fingerprint_string(completion["input_fingerprint_fnv1a64"])
        ):
            raise ValueError(f"{label} completion input is not exact")
        serial_completion = _precise_normalize_compact_bundle(
            completion["serial_reference"],
            role,
            audit,
            f"{label}.completion.serial",
            expected_record_count=12,
            expected_samples=completion_samples,
            worker_count=1,
        )
        completion_orders_value = exact_list(
            completion["orders"], 6, f"{label}.completion.orders"
        )
        completion_order_sha256: dict[str, str] = {
            "serial": canonical_sha256(serial_completion)
        }
        completion_orchestration_sha256: dict[str, str] = {
            "serial": canonical_sha256(
                _precise_orchestration_projection(serial_completion)
            )
        }
        for index, (expected_order, order_value) in enumerate(
            zip(
                PRECISE_REFERENCE_COMPLETION_ORDERS,
                completion_orders_value,
                strict=True,
            )
        ):
            order = exact_object(
                order_value, ("order", "transcript"),
                f"{label}.completion.orders[{index}]"
            )
            if order["order"] != list(expected_order):
                raise ValueError(f"{label} completion order inventory drifted")
            transcript = _precise_normalize_compact_bundle(
                order["transcript"],
                role,
                audit,
                f"{label}.completion.orders[{index}]",
                expected_record_count=12,
                expected_samples=completion_samples,
                worker_count=3,
            )
            completion_order_sha256["-".join(map(str, expected_order))] = (
                canonical_sha256(transcript)
            )
            completion_orchestration_sha256[
                "-".join(map(str, expected_order))
            ] = canonical_sha256(
                _precise_orchestration_projection(transcript)
            )
        worker_three_completion_hashes = {
            value
            for key, value in completion_order_sha256.items()
            if key != "serial"
        }
        if len(worker_three_completion_hashes) != 1:
            raise ValueError(
                f"{label} completion permutations differ at matched worker count"
            )
        if len(set(completion_orchestration_sha256.values())) != 1:
            raise ValueError(
                f"{label} completion orchestration projection differs"
            )

        exceptional_values = exact_list(
            evidence["exceptional_cases"], 3, f"{label}.exceptional_cases"
        )
        exceptional_order = (
            "pre_cancelled",
            "expired_absolute_deadline",
            "worker_exception",
        )
        exceptional_sha256: dict[str, str] = {}
        exceptional_inputs: list[dict[str, Any]] = []
        exceptional_samples = [_precise_sample(False, None) for _ in range(8)]
        for index, (case_id, case_value) in enumerate(
            zip(exceptional_order, exceptional_values, strict=True)
        ):
            exceptional = exact_object(
                case_value,
                (
                    "case_id",
                    "worker_count",
                    "input_waypoint_count",
                    "control_condition",
                    "throwing_worker_index",
                    "absolute_deadline_ns",
                    "pre_cancel_reason",
                    "fallback_reason",
                    "expected",
                    "observed",
                ),
                f"{label}.exceptional_cases[{index}]",
            )
            controls = {
                "pre_cancelled": (
                    "pre_cancelled_operator_stop", None, None, "operator_stop"
                ),
                "expired_absolute_deadline": (
                    "already_expired_absolute_deadline_zero", None, 0, None
                ),
                "worker_exception": (
                    "worker_index_1_throws_std_exception", 1, None, None
                ),
            }[case_id]
            expected = _precise_expected_exception(case_id)
            if not (
                exceptional["case_id"] == case_id
                and exceptional["worker_count"] == 2
                and exceptional["input_waypoint_count"] == 8
                and (
                    exceptional["control_condition"],
                    exceptional["throwing_worker_index"],
                    exceptional["absolute_deadline_ns"],
                    exceptional["pre_cancel_reason"],
                )
                == controls
                and exceptional["fallback_reason"] == expected["fallback_reason"]
                and exceptional["expected"] == expected
            ):
                raise ValueError(f"{label} exceptional contract {case_id} is invalid")
            expected_count = {"pre_cancelled": 0, "expired_absolute_deadline": 1, "worker_exception": 5}[case_id]
            observed = _precise_normalize_compact_bundle(
                exceptional["observed"],
                role,
                audit,
                f"{label}.exceptional.{case_id}",
                expected_record_count=expected_count,
                semantic_records=False,
                expected_samples=exceptional_samples,
                worker_count=2,
                expected_exceptional_volatile_key=(
                    "elapsed_ns"
                    if case_id == "expired_absolute_deadline"
                    else None
                ),
            )
            decision = observed["decision"]
            if tuple(
                decision[field]
                for field in (
                    "accepted",
                    "action_code",
                    "severity_code",
                    "first_failing_waypoint",
                    "first_failing_time_ns",
                )
            ) != tuple(
                expected[field]
                for field in (
                    "accepted",
                    "action_code",
                    "severity_code",
                    "first_failing_waypoint",
                    "first_failing_time_ns",
                )
            ):
                raise ValueError(f"{label} exceptional decision {case_id} drifted")
            records = observed["waypoint_records"]
            terminal_status = records[-1]["engine_status_code"] if records else None
            if terminal_status != expected["engine_status_code"]:
                raise ValueError(f"{label} exceptional terminal status {case_id} drifted")
            if case_id == "pre_cancelled":
                observed_reason = observed["cancellation_reason"]
            else:
                observed_reason = records[-1]["diagnostics"].get("reason_code")
            if observed_reason != expected["reason"]:
                raise ValueError(f"{label} exceptional reason {case_id} drifted")
            exceptional_inputs.append(
                {
                    "case_id": case_id,
                    "worker_count": 2,
                    "input_waypoint_count": 8,
                    "control_condition": controls[0],
                    "throwing_worker_index": controls[1],
                    "absolute_deadline_ns": controls[2],
                    "pre_cancel_reason": controls[3],
                }
            )
            exceptional_sha256[case_id] = canonical_sha256(observed)

        if small_inputs is None or partition_inputs is None:
            raise ValueError(f"{label} normalized input corpus is empty")
        input_corpus = {
            "small_cases": small_inputs,
            "partition": {
                "encoding": "little_endian_base4",
                "sample_kinds": expected_sample_kinds,
                "patterns": partition_inputs,
            },
            "completion": {
                "signed_clearances_m": list(
                    PRECISE_REFERENCE_COMPLETION_CLEARANCES
                ),
                "orders": [
                    list(order) for order in PRECISE_REFERENCE_COMPLETION_ORDERS
                ],
            },
            "exceptional_cases": exceptional_inputs,
        }
        normalized_output_inventory = {
            "small_case_profile_sha256": small_profile_sha256,
            "small_orchestration_profile_sha256": (
                small_orchestration_profile_sha256
            ),
            "small_case_sha256": small_case_sha256,
            "partition_profile_sha256": partition_profile_sha256,
            "partition_orchestration_profile_sha256": (
                partition_orchestration_profile_sha256
            ),
            "completion_order_sha256": completion_order_sha256,
            "completion_orchestration_sha256": (
                completion_orchestration_sha256
            ),
            "exceptional_case_sha256": exceptional_sha256,
        }
        result = {
            "input_corpus_sha256": canonical_sha256(input_corpus),
            "normalized_transcript_sha256": canonical_sha256(
                normalized_output_inventory
            ),
            "normalization_contract_sha256": canonical_sha256(contract),
            "orchestration_projection_contract_sha256": canonical_sha256(
                orchestration_contract
            ),
            "small_case_profile_sha256": small_profile_sha256,
            "small_orchestration_profile_sha256": (
                small_orchestration_profile_sha256
            ),
            "small_case_sha256": small_case_sha256,
            "partition_profile_sha256": partition_profile_sha256,
            "partition_orchestration_profile_sha256": (
                partition_orchestration_profile_sha256
            ),
            "completion_order_sha256": completion_order_sha256,
            "completion_orchestration_sha256": (
                completion_orchestration_sha256
            ),
            "exceptional_case_sha256": exceptional_sha256,
            "normalization_audit": audit,
        }
    except (KeyError, TypeError, ValueError) as error:
        checks.require(
            f"{label}:precise_reference_closed_semantics", False, str(error)
        )
        return None
    checks.require(
        f"{label}:precise_reference_closed_semantics", True,
        f"normalized={result['normalized_transcript_sha256']}"
    )
    return role, result


def validate_lightweight(record: dict[str, Any], checks: Checks) -> tuple[str, int] | None:
    label = record_label(record)
    identity = record.get("identity", {})
    value = record.get("payload", {})
    arm = value.get("query_class")
    waypoints = value.get("batch_size")
    active_pairs = value.get("active_collision_pair_count")
    colliding_geometry_pairs = value.get("colliding_geometry_pair_count")
    colliding_logical_pairs = value.get("colliding_logical_pair_count")
    valid_waypoints = (
        waypoints
        if positive_integer(waypoints) and waypoints in LIGHTWEIGHT_WAYPOINTS
        else None
    )
    checks.require(
        f"{label}:current_only",
        identity.get("source_role") == CURRENT_ROLE,
    )
    checks.require(
        f"{label}:fixture_and_arm",
        value.get("record_type") == "collision_query_batch"
        and value.get("fixture_id") == LIGHTWEIGHT_FIXTURE
        and arm in {"binary_contact", "collision_only"}
        and waypoints in LIGHTWEIGHT_WAYPOINTS,
    )
    checks.require(
        f"{label}:process_profile",
        value.get("warmup_count") == WARMUP_RUNS
        and value.get("sample_count") == MEASURED_RUNS
        and valid_waypoints is not None
        and value.get("state_query_count") == MEASURED_RUNS * valid_waypoints,
    )
    checks.require(
        f"{label}:decision_and_mechanism",
        value.get("passed") is True
        and value.get("mechanism_matches") is True
        and value.get("policy_scope_matches") is True
        and value.get("collision_api_call_count") == 1
        and value.get("collision_enumeration_completion_ordinal") == 1
        and value.get("fk_placement_invocation_count") == 1
        and positive_integer(active_pairs)
        and positive_integer(colliding_geometry_pairs)
        and positive_integer(colliding_logical_pairs)
        and active_pairs > colliding_geometry_pairs >= colliding_logical_pairs
        and positive_integer(value.get("normalized_pair_count")),
    )
    checks.require(
        f"{label}:physical_output",
        isinstance(value.get("physical_output_in_collision"), bool)
        and isinstance(value.get("physical_output_engine_status_code"), int)
        and not isinstance(value.get("physical_output_engine_status_code"), bool)
        and isinstance(value.get("physical_output_decision_code"), int)
        and not isinstance(value.get("physical_output_decision_code"), bool)
        and positive_integer(value.get("physical_output_normalized_pair_count"))
        and value.get("physical_output_normalized_pair_count")
        == value.get("normalized_pair_count")
        and fingerprint_string(
            value.get("physical_output_normalized_pair_fingerprint")
        ),
    )
    checks.require(
        f"{label}:zero_distance",
        value.get("distance_request_mode") == "none"
        and value.get("selected_distance_pair_count") == 0
        and value.get("clearance_pair_count") == 0
        and value.get("distance_api_call_count") == 0
        and value.get("first_distance_evaluation_ordinal") == 0
        and value.get("distance_evaluated_geometry_pair_count") == 0
        and value.get("distance_evaluated_logical_pair_count") == 0
        and value.get("min_clearance_m") is None,
    )
    if arm == "binary_contact":
        policy_ok = (
            value.get("effective_contact_policy_mode") == "contact_release"
            and value.get("effective_release_evidence_mode") == "binary_contact"
        )
    else:
        policy_ok = (
            value.get("effective_contact_policy_mode") is None
            and value.get("effective_release_evidence_mode") is None
        )
    checks.require(f"{label}:mode_honesty", policy_ok)
    checks.require(
        f"{label}:timing_shape",
        finite_positive(nested(value, "latency_ns", "p95"))
        and finite_positive(nested(value, "cpu_time_ns", "p95"))
        and finite_positive(nested(value, "per_state_latency_ns", "p95"))
        and finite_positive(nested(value, "per_state_cpu_time_ns", "p95")),
    )
    return (
        (arm, valid_waypoints)
        if arm in {"binary_contact", "collision_only"}
        and valid_waypoints is not None
        else None
    )


def validate_contact(
    record: dict[str, Any], checks: Checks
) -> tuple[str, int, int] | None:
    label = record_label(record)
    identity = record.get("identity", {})
    provenance = record.get("provenance", {})
    value = record.get("payload", {})
    role = identity.get("source_role")
    waypoints = value.get("waypoint_count")
    workers = value.get("worker_count_requested")
    checks.require(
        f"{label}:schema_and_fixture",
        value.get("schema_version") == CONTACT_SCHEMA
        and value.get("record_type") == "contact_grasp_validation"
        and value.get("fixture_id") == CONTACT_FIXTURE
        and role in {BASELINE_ROLE, CURRENT_ROLE}
        and waypoints in WAYPOINTS,
    )
    embedded_identity_fields = (
        "source_role",
        "image_id",
        "binary_sha256",
        "run_id",
        "requested_cpu_affinity",
        "process_instance_id",
    )
    checks.require(
        f"{label}:embedded_identity_matches_envelope",
        all(value.get(field) == identity.get(field) for field in embedded_identity_fields)
        and value.get("source_identity") == identity.get("source_identity")
        and value.get("specs_source_identity")
        == identity.get("specs_source_identity")
        and value.get("specs_contract_identity")
        == identity.get("specs_contract_identity"),
    )
    checks.require(
        f"{label}:embedded_baseline_provenance_matches_envelope",
        value.get("clean_checkout_manifest_sha256")
        == provenance.get("clean_checkout_manifest_sha256")
        and value.get("qualification_patch_sha256")
        == provenance.get("qualification_patch_sha256"),
    )
    checks.require(
        f"{label}:matched_profile",
        workers in CONTACT_WORKER_PROFILES
        and value.get("worker_count_effective") == workers
        and value.get("workers_used") == 1
        and value.get("fallback_reason")
        == (
            "worker_count_one"
            if workers == 1
            else "contact_grasp_policy_active"
        )
        and value.get("warmup_runs") == WARMUP_RUNS
        and value.get("measured_runs") == MEASURED_RUNS,
    )
    normalized_fingerprint = value.get("normalized_pair_output_fingerprint")
    clearance_fingerprint = value.get("colliding_clearance_output_fingerprint")
    queried_engines = value.get("queried_engine_count")
    collision_calls = value.get("collision_api_call_count")
    active_pairs = value.get("active_collision_pair_count")
    colliding_geometry_pairs = value.get("colliding_geometry_pair_count")
    colliding_logical_pairs = value.get("colliding_logical_pair_count")
    distance_calls = value.get("distance_api_call_count")
    distance_geometry_pairs = value.get(
        "distance_evaluated_geometry_pair_count"
    )
    distance_logical_pairs = value.get("distance_evaluated_logical_pair_count")
    checks.require(
        f"{label}:decision_and_physical_output",
        value.get("passed") is True
        and value.get("decision_accepted") is True
        and value.get("initial_waypoint_clear_by_construction") is True
        and value.get("identical_contact_state_after_initial_waypoint") is True
        and value.get("initial_waypoint_clear") is True
        and value.get("every_contact_waypoint_in_collision") is True
        and value.get("waypoint_record_count") == waypoints
        and value.get("normalized_pair_observation_count", 0) > 0
        and value.get("clearance_observation_count", 0) > 0
        and value.get("finite_colliding_clearance_observation_count")
        == value.get("normalized_pair_observation_count")
        and finite_nonnegative(value.get("maximum_observed_penetration_m"))
        and value.get("maximum_observed_penetration_m") <= 0.5
        and fingerprint_string(normalized_fingerprint)
        and fingerprint_string(clearance_fingerprint)
        and value.get("allowed_robot_link_count", 0) > 0
        and value.get("max_penetration_m") == 0.5,
    )
    if role == BASELINE_ROLE:
        mechanism_ok = (
            value.get("mechanism_provenance") == "not_available_pristine_timing"
            and value.get("distance_request_mode") is None
            and value.get("mechanism_checked") is False
            and positive_integer(value.get("initial_waypoint_clearance_count"))
            and all(
                value.get(field) is None
                for field in (
                    "queried_engine_count",
                    "collision_api_call_count",
                    "collision_enumeration_completion_ordinal",
                    "first_distance_evaluation_ordinal",
                    "active_collision_pair_count",
                    "colliding_geometry_pair_count",
                    "colliding_logical_pair_count",
                    "distance_api_call_count",
                    "distance_evaluated_geometry_pair_count",
                    "distance_evaluated_logical_pair_count",
                    "mechanism_matches",
                )
            )
        )
    else:
        mechanism_ok = (
            value.get("mechanism_provenance") == "direct_current"
            and value.get("distance_request_mode") == "selected_colliding_pairs"
            and value.get("mechanism_checked") is True
            and value.get("initial_waypoint_clearance_count") == 0
            and positive_integer(queried_engines)
            and queried_engines == value.get("workers_used")
            and collision_calls == queried_engines
            and value.get("collision_enumeration_completion_ordinal") == 1
            and value.get("first_distance_evaluation_ordinal") == 2
            and positive_integer(active_pairs)
            and positive_integer(colliding_geometry_pairs)
            and positive_integer(colliding_logical_pairs)
            and active_pairs > colliding_geometry_pairs
            and distance_calls
            == distance_geometry_pairs
            == colliding_geometry_pairs
            and distance_logical_pairs == colliding_logical_pairs
            and value.get("mechanism_matches") is True
        )
    checks.require(f"{label}:mechanism_scope", mechanism_ok)
    checks.require(
        f"{label}:timing_shape",
        finite_positive(nested(value, "wall_us", "p95"))
        and finite_positive(nested(value, "process_cpu_us", "p95")),
    )
    return (
        (role, waypoints, workers)
        if role in {BASELINE_ROLE, CURRENT_ROLE}
        and waypoints in WAYPOINTS
        and workers in CONTACT_WORKER_PROFILES
        else None
    )


def validate_policy_support(
    record: dict[str, Any], checks: Checks
) -> tuple[str, str, int, int] | None:
    label = record_label(record)
    identity = record.get("identity", {})
    provenance = record.get("provenance", {})
    value = record.get("payload", {})
    role = identity.get("source_role")
    arm = value.get("arm")
    waypoints = value.get("waypoint_count")
    workers = value.get("worker_count_requested")
    expected_arm = (
        arm == "current_all_active_distance"
        if role == BASELINE_ROLE
        else arm
        in {
            "signed_penetration_selective",
            "binary_contact",
            "bounded_penetration",
            "collision_only",
        }
    )
    checks.require(
        f"{label}:schema_fixture_arm",
        value.get("schema_version") == POLICY_SCHEMA
        and value.get("record_type") == "policy_support_contact_validation"
        and value.get("fixture_id") == POLICY_FIXTURE
        and value.get("fixture_variant") == POLICY_VARIANT
        and value.get("fixture_world_profile") == "support_contact"
        and role in {BASELINE_ROLE, CURRENT_ROLE}
        and expected_arm
        and waypoints in POLICY_WAYPOINTS
        and workers in CONTACT_WORKER_PROFILES,
    )
    embedded_identity_fields = (
        "source_role",
        "image_id",
        "binary_sha256",
        "run_id",
        "requested_cpu_affinity",
        "process_instance_id",
    )
    checks.require(
        f"{label}:embedded_identity",
        all(value.get(field) == identity.get(field) for field in embedded_identity_fields)
        and value.get("source_identity") == identity.get("source_identity")
        and value.get("specs_source_identity")
        == identity.get("specs_source_identity")
        and value.get("specs_contract_identity")
        == identity.get("specs_contract_identity")
        and value.get("clean_checkout_manifest_sha256")
        == provenance.get("clean_checkout_manifest_sha256")
        and value.get("qualification_patch_sha256")
        == provenance.get("qualification_patch_sha256"),
    )
    checks.require(
        f"{label}:matched_input_provenance",
        sha256_string(value.get("corpus_sha256"))
        and isinstance(value.get("snapshot_content_hash"), str)
        and bool(value.get("snapshot_content_hash"))
        and fingerprint_string(value.get("snapshot_geometry_fingerprint"))
        and fingerprint_string(value.get("state_fingerprint"))
        and fingerprint_string(value.get("trajectory_fingerprint"))
        and fingerprint_string(value.get("configured_pair_fingerprint"))
        and fingerprint_string(value.get("physical_collision_pair_fingerprint"))
        and positive_integer(value.get("body_count"))
        and positive_integer(value.get("geometry_count"))
        and value.get("distractor_count") == 10
        and positive_integer(value.get("configured_contact_pair_count")),
    )
    checks.require(
        f"{label}:actual_process_environment",
        isinstance(value.get("effective_cpu_affinity"), str)
        and bool(value.get("effective_cpu_affinity"))
        and isinstance(value.get("scheduler_policy"), str)
        and value.get("scheduler_policy") != "SCHED_UNKNOWN"
        and isinstance(value.get("scheduler_priority"), int)
        and isinstance(value.get("process_nice"), int)
        and isinstance(value.get("kernel_id"), str)
        and bool(value.get("kernel_id"))
        and isinstance(value.get("cpu_governor_set"), str)
        and bool(value.get("cpu_governor_set"))
        and isinstance(value.get("cpu_frequency_driver_set"), str)
        and bool(value.get("cpu_frequency_driver_set"))
        and isinstance(value.get("compiler"), str)
        and bool(value.get("compiler"))
        and value.get("build_type") == "Release"
        and value.get("compiler_flags") == "-Wno-error=maybe-uninitialized",
    )
    runner_environment = provenance.get("runner_environment", {})
    command = provenance.get("command_argv", [])
    outer_command = provenance.get("outer_command_argv", [])
    runner_command = provenance.get("runner_command_argv", [])
    checks.require(
        f"{label}:exact_runner_environment_binding",
        isinstance(runner_environment, dict)
        and runner_environment.get("requested_cpu_affinity")
        == value.get("effective_cpu_affinity")
        and runner_environment.get("image_id") == value.get("image_id")
        and runner_environment.get("compiler") == value.get("compiler")
        and runner_environment.get("compiler_flags") == value.get("compiler_flags")
        and runner_environment.get("build_type") == value.get("build_type")
        and runner_environment.get("cpu_governor_set")
        == value.get("cpu_governor_set")
        and runner_environment.get("cpu_frequency_driver_set")
        == value.get("cpu_frequency_driver_set")
        and runner_environment.get("kernel_id") == value.get("kernel_id")
        and runner_environment.get("policy_support_corpus_sha256")
        == value.get("corpus_sha256")
        and isinstance(outer_command, list)
        and isinstance(command, list)
        and outer_command[-len(command):] == command
        and isinstance(runner_command, list)
        and "--cell" in runner_command
        and "policy-support-contact" in runner_command,
    )
    expected_workers_used = workers
    checks.require(
        f"{label}:matched_profile",
        value.get("worker_count_effective") == workers
        and value.get("workers_used") == expected_workers_used
        and value.get("fallback_reason")
        == ("worker_count_one" if workers == 1 else "")
        and value.get("warmup_runs") == WARMUP_RUNS
        and value.get("measured_runs") == MEASURED_RUNS,
    )
    pair_count = value.get("configured_contact_pair_count")
    physical_pair_count = value.get("physical_collision_pair_count")
    incidental_pair_count = value.get("incidental_allowed_pair_count")
    expected_metric_observations = (
        pair_count * waypoints
        if positive_integer(pair_count) and positive_integer(waypoints)
        else None
    )
    expected_physical_observations = (
        physical_pair_count * waypoints
        if positive_integer(physical_pair_count) and positive_integer(waypoints)
        else None
    )
    distance_mode = arm in {
        "current_all_active_distance",
        "signed_penetration_selective",
        "bounded_penetration",
    }
    selective_mode = arm in {
        "signed_penetration_selective",
        "bounded_penetration",
    }
    clearances = value.get("clearance_observation_count")
    finite_clearances = value.get("finite_colliding_clearance_observation_count")
    checks.require(
        f"{label}:decision_physical_output",
        value.get("passed") is True
        and value.get("decision_accepted") is True
        and value.get("every_waypoint_in_collision") is True
        and value.get("waypoint_record_count") == waypoints
        and positive_integer(physical_pair_count)
        and physical_pair_count > pair_count
        and incidental_pair_count == physical_pair_count - pair_count
        and value.get("normalized_pair_observation_count")
        == expected_physical_observations
        and fingerprint_string(value.get("normalized_pair_output_fingerprint"))
        and fingerprint_string(value.get("colliding_clearance_output_fingerprint"))
        and isinstance(value.get("decision_action_code"), int)
        and isinstance(value.get("decision_severity_code"), int),
    )
    if arm == "current_all_active_distance":
        clearance_ok = (
            positive_integer(clearances)
            and clearances > expected_metric_observations
            and finite_clearances == expected_physical_observations
            and finite_positive(value.get("maximum_observed_penetration_m"))
        )
    elif selective_mode:
        clearance_ok = (
            clearances == expected_metric_observations
            and finite_clearances == expected_metric_observations
            and finite_positive(value.get("maximum_observed_penetration_m"))
        )
    else:
        clearance_ok = (
            clearances == 0
            and finite_clearances == 0
            and value.get("maximum_observed_penetration_m") == 0
        )
    checks.require(f"{label}:clearance_scope", clearance_ok)
    query_count = value.get("mechanism_query_count")
    collision_calls = value.get("collision_api_call_count")
    active_pairs = value.get("active_collision_pair_count")
    colliding_geometry = value.get("colliding_geometry_pair_count")
    colliding_logical = value.get("colliding_logical_pair_count")
    distance_calls = value.get("distance_api_call_count")
    distance_geometry = value.get("distance_evaluated_geometry_pair_count")
    distance_logical = value.get("distance_evaluated_logical_pair_count")
    expected_fk = waypoints if role == CURRENT_ROLE else 2 * waypoints
    mechanism_ok = (
        value.get("mechanism_checked") is True
        and value.get("mechanism_timing_isolated") is True
        and query_count == waypoints
        and collision_calls == waypoints
        and value.get("queried_engine_count") == value.get("mechanism_workers_used")
        and value.get("mechanism_workers_used") == expected_workers_used
        and positive_integer(active_pairs)
        and active_pairs > colliding_geometry
        and colliding_geometry
        == colliding_logical
        == expected_physical_observations
        and value.get("fk_placement_invocation_count") == expected_fk
    )
    if arm == "current_all_active_distance":
        mechanism_ok = (
            mechanism_ok
            and value.get("distance_request_mode") == "all_active_pairs"
            and distance_calls == waypoints
            and distance_geometry == distance_logical == clearances
            and distance_logical > colliding_logical
            and value.get("mechanism_provenance")
            == "exact_result_and_source_derivation_pristine_2bfe30e"
            and value.get("fk_placement_provenance")
            == "exact_pristine_source_derivation_collision_plus_bulk_distance"
            and value.get("fk_placement_derivation_source")
            == "manyforge_collision/src/manyforge_collision_engine.cpp@2bfe30eeb37952f1ddc36bfdbf89d471a9f6b909:1221,1231"
        )
    elif selective_mode:
        mechanism_ok = (
            mechanism_ok
            and value.get("distance_request_mode") == "selected_colliding_pairs"
            and distance_calls == distance_geometry
            and distance_geometry
            == distance_logical
            == expected_metric_observations
            and value.get("mechanism_provenance")
            == "direct_current_full_trajectory_observation"
            and value.get("fk_placement_provenance")
            == "direct_current_full_trajectory_counter"
            and value.get("fk_placement_derivation_source") is None
        )
    else:
        mechanism_ok = (
            mechanism_ok
            and value.get("distance_request_mode") == "none"
            and distance_calls == distance_geometry == distance_logical == 0
            and value.get("mechanism_provenance")
            == "direct_current_full_trajectory_observation"
            and value.get("fk_placement_derivation_source") is None
        )
    checks.require(f"{label}:full_trajectory_mechanism", mechanism_ok)
    fields = value.get("policy_fields")
    common_policy_ok = (
        value.get("policy_id") == "support/step18_policy_perf"
        and fingerprint_string(value.get("policy_content_hash"))
        and isinstance(fields, dict)
        and fields.get("require_full_diagnostics") is True
    )
    if arm == "collision_only":
        policy_ok = (
            common_policy_ok
            and fields.get("support_contact_count") == 0
            and fields.get("allowed_touch_pair_count") == physical_pair_count
            and all(
                fields.get(name) is None
                for name in (
                    "mode",
                    "release_evidence_mode",
                    "enforce_max_start_penetration",
                    "max_contact_penetration_m",
                    "bounded_initial_contact_whitelist",
                )
            )
        )
    else:
        expected_evidence = {
            "current_all_active_distance": None,
            "signed_penetration_selective": "signed_penetration",
            "binary_contact": "binary_contact",
            "bounded_penetration": None,
        }[arm]
        bounded = arm == "bounded_penetration"
        binary = arm == "binary_contact"
        policy_ok = (
            common_policy_ok
            and fields.get("support_contact_count") == pair_count
            and fields.get("allowed_touch_pair_count") == incidental_pair_count
            and fields.get("mode")
            == ("bounded_penetration" if bounded else "contact_release")
            and fields.get("release_evidence_mode") == expected_evidence
            and fields.get("enforce_max_start_penetration")
            is (not bounded and not binary)
            and fields.get("max_start_penetration_m")
            == (0 if bounded or binary else 0.03)
            and fields.get("release_tolerance_m")
            == (0 if bounded or binary else 0.001)
            and fields.get("min_release_progress_m") == 0
            and fields.get("require_separation_by_end") is False
            and fields.get("require_strictly_decreasing_penetration") is False
            and fields.get("allow_recontact") is (not bounded)
            and fields.get("max_contact_penetration_m")
            == (0.03 if bounded else None)
            and fields.get("bounded_initial_contact_whitelist")
            is (False if bounded else None)
        )
    checks.require(f"{label}:canonical_policy_fields", policy_ok)
    bounded_negative_ok = (
        value.get("bounded_negative_checked") is True
        and finite_positive(value.get("bounded_negative_cap_m"))
        and value.get("bounded_negative_cap_m")
        < value.get("maximum_observed_penetration_m")
        and value.get("bounded_negative_rejected") is True
        and value.get("bounded_negative_first_failing_waypoint") == 0
        if arm == "bounded_penetration"
        else value.get("bounded_negative_checked") is False
        and value.get("bounded_negative_cap_m") is None
        and value.get("bounded_negative_rejected") is None
        and value.get("bounded_negative_first_failing_waypoint") is None
    )
    checks.require(f"{label}:bounded_negative_outside_timing", bounded_negative_ok)
    checks.require(
        f"{label}:typed_auxiliary_observations",
        value.get("allocation_count") is None
        and value.get("allocation_observation_status")
        == "not_observed_in_this_timing_cell"
        and value.get("snapshot_sync_time_us") is None
        and value.get("snapshot_sync_observation_status")
        == "not_applicable_immutable_prebuilt_snapshot"
        and value.get("cancellation_status") == "not_requested"
        and value.get("timeout_status") == "not_configured",
    )
    checks.require(
        f"{label}:timing_shape",
        finite_positive(nested(value, "wall_us", "p95"))
        and finite_positive(nested(value, "process_cpu_us", "p95")),
    )
    return (
        (role, arm, waypoints, workers)
        if role in {BASELINE_ROLE, CURRENT_ROLE}
        and expected_arm
        and waypoints in POLICY_WAYPOINTS
        and workers in CONTACT_WORKER_PROFILES
        else None
    )


def fixture_map(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    fixtures = payload.get("fixtures")
    if not isinstance(fixtures, list):
        return {}
    return {
        fixture.get("fixture"): fixture
        for fixture in fixtures
        if isinstance(fixture, dict) and isinstance(fixture.get("fixture"), str)
    }


def worker_map(fixture: dict[str, Any]) -> dict[tuple[int, int, int], dict[str, Any]]:
    workers = nested(fixture, "validation", "worker_sweep", default=[])
    if not isinstance(workers, list):
        return {}
    result: dict[tuple[int, int, int], dict[str, Any]] = {}
    for worker in workers:
        if not isinstance(worker, dict):
            continue
        key = (
            worker.get("workers_requested"),
            worker.get("workers_effective"),
            worker.get("workers_used"),
        )
        if all(isinstance(value, int) and value > 0 for value in key):
            result[key] = worker
    return result


def validate_free_space(record: dict[str, Any], checks: Checks) -> str | None:
    label = record_label(record)
    identity = record.get("identity", {})
    role = identity.get("source_role")
    value = record.get("payload", {})
    fixtures = fixture_map(value)
    checks.require(
        f"{label}:matrix_schema",
        value.get("phase4_schema") == 1
        and value.get("qualification_scope") == "collision_engine_fixture_matrix"
        and value.get("warmup_runs") == WARMUP_RUNS
        and value.get("measured_runs") == MEASURED_RUNS
        and value.get("all_passed") is True
        and value.get("fixtures_passed") == value.get("fixture_count")
        and role in {BASELINE_ROLE, CURRENT_ROLE},
    )
    fixture_list = value.get("fixtures")
    if role == CURRENT_ROLE:
        checks.require(
            f"{label}:exact_current_nine_fixture_matrix",
            isinstance(fixture_list, list)
            and len(fixture_list) == 9
            and value.get("fixture_count") == 9
            and set(fixtures) == set(CURRENT_FREE_SPACE_FIXTURES),
        )
    else:
        checks.require(
            f"{label}:baseline_common_fixtures",
            all(name in fixtures for name in BASELINE_FREE_SPACE_FIXTURES),
        )
    for name, fixture in fixtures.items():
        checks.require(
            f"{label}:{name}:fixed_measurement_profile",
            fixture.get("query_warmup_runs") == WARMUP_RUNS
            and fixture.get("query_measured_runs") == MEASURED_RUNS
            and nested(fixture, "validation", "warmup_runs") == WARMUP_RUNS
            and nested(fixture, "validation", "measured_runs") == MEASURED_RUNS
            and nested(fixture, "noncollision_validation", "warmup_runs")
            == WARMUP_RUNS
            and nested(fixture, "noncollision_validation", "measured_runs")
            == MEASURED_RUNS,
        )
    for name in BASELINE_FREE_SPACE_FIXTURES:
        fixture = fixtures.get(name, {})
        workers = worker_map(fixture)
        checks.require(
            f"{label}:{name}:decision",
            fixture.get("passed") is True
            and fixture.get("decision_match") is True
            and nested(fixture, "validation", "enabled") is True
            and nested(fixture, "validation", "decision_match") is True
            and nested(fixture, "validation", "waypoints") == 96
            and nested(fixture, "validation", "warmup_runs") == WARMUP_RUNS
            and nested(fixture, "validation", "measured_runs") == MEASURED_RUNS,
        )
        checks.require(
            f"{label}:{name}:worker_sweep",
            len(workers) == 4
            and set(key[0] for key in workers) == {1, 2, 3, 4}
            and all(
                finite_positive(nested(worker, "wall_us", "p95"))
                and finite_positive(nested(worker, "process_cpu_us", "p95"))
                and worker.get("decision_match") is True
                for worker in workers.values()
            ),
        )
        if role == CURRENT_ROLE:
            checks.require(
                f"{label}:{name}:current_mechanism",
                all(
                    worker.get("distance_request_mode") == "none"
                    and worker.get("distance_scope_verified") is True
                    and worker.get("distance_api_call_count") == 0
                    and worker.get("distance_evaluated_logical_pair_count") == 0
                    for worker in workers.values()
                ),
            )
    if role == CURRENT_ROLE:
        binary = fixtures.get("binary_contact_ordered_summary", {})
        bounded = fixtures.get("bounded_penetration_insertion", {})
        binary_workers = worker_map(binary)
        bounded_workers = worker_map(bounded)
        checks.require(
            f"{label}:binary_fixture_bound",
            binary.get("passed") is True
            and binary.get("support_contact_ordered_summary_verified") is True
            and nested(binary, "contact_policy", "mode") == "contact_release"
            and nested(binary, "contact_policy", "release_evidence_mode")
            == "binary_contact"
            and bool(binary_workers)
            and all(
                worker.get("distance_request_mode") == "none"
                and worker.get("distance_scope_verified") is True
                and worker.get("distance_api_call_count") == 0
                and worker.get("distance_evaluated_logical_pair_count") == 0
                for worker in binary_workers.values()
            ),
        )
        checks.require(
            f"{label}:bounded_fixture_bound",
            bounded.get("passed") is True
            and bounded.get("support_contact_ordered_summary_verified") is True
            and nested(bounded, "contact_policy", "mode")
            == "bounded_penetration"
            and nested(bounded, "contact_policy", "release_evidence_mode")
            == "not_applicable"
            and finite_positive(
                nested(bounded, "contact_policy", "max_contact_penetration_m")
            )
            and nested(bounded, "contact_policy", "bounded_over_cap_verified")
            is True
            and bool(bounded_workers)
            and all(
                worker.get("distance_request_mode")
                == "selected_colliding_pairs"
                and worker.get("distance_scope_verified") is True
                and positive_integer(worker.get("distance_api_call_count"))
                and positive_integer(
                    worker.get("distance_evaluated_logical_pair_count")
                )
                for worker in bounded_workers.values()
            ),
        )
    return role if role in {BASELINE_ROLE, CURRENT_ROLE} else None


def main() -> int:
    args = parse_args()
    if args.policy_support_only and not args.development_smoke:
        raise SystemExit(
            "--policy-support-only is a development sub-check and requires "
            "--development-smoke"
        )
    if args.development_smoke:
        if args.minimum_runs != 1 or not args.policy_support_only:
            raise SystemExit(
                "--development-smoke requires --policy-support-only --minimum-runs 1"
            )
    elif args.minimum_runs != 3:
        raise SystemExit("formal --minimum-runs must be exactly 3")
    checks = Checks()
    try:
        records = [load_artifact(path) for path in args.artifacts]
        clean_manifest_bytes, clean_manifest_hash = read_file_snapshot(
            args.baseline_clean_checkout_manifest
        )
        clean_manifest = json.loads(clean_manifest_bytes)
        qualification_patch_bytes, qualification_patch_hash = read_file_snapshot(
            args.baseline_qualification_patch
        )
        qualification_paths = patch_paths_from_bytes(
            qualification_patch_bytes, args.baseline_qualification_patch
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"proof-cell input failed: {error}", file=sys.stderr)
        return 2
    artifact_manifest = []
    for record in records:
        payload = record.get("payload", {})
        artifact_manifest.append(
            {
                "role": "proof_cell_artifact",
                **record["_artifact"],
                "cell": record.get("cell"),
                "source_role": nested(record, "identity", "source_role"),
                "run_id": nested(record, "identity", "run_id"),
                "process_instance_id": nested(
                    record, "identity", "process_instance_id"
                ),
                "arm": (
                    payload.get("arm", payload.get("query_class"))
                    if isinstance(payload, dict)
                    else None
                ),
                "waypoints": (
                    payload.get("waypoint_count", payload.get("batch_size"))
                    if isinstance(payload, dict)
                    else None
                ),
                "workers": (
                    payload.get("worker_count_requested")
                    if isinstance(payload, dict)
                    else None
                ),
            }
        )
    artifact_manifest.extend(
        (
            {
                "role": "baseline_clean_checkout_manifest",
                "path": str(args.baseline_clean_checkout_manifest.resolve()),
                "sha256": clean_manifest_hash,
            },
            {
                "role": "baseline_qualification_patch",
                "path": str(args.baseline_qualification_patch.resolve()),
                "sha256": qualification_patch_hash,
            },
        )
    )
    verifier_path = Path(__file__).resolve()
    scripts_root = verifier_path.parents[1]
    tool_paths = {
        "proof_verifier": verifier_path,
        "proof_wrapper": verifier_path.with_name("wrap_step18_proof_artifact.py"),
        "source_identity_helper": verifier_path.with_name("source_identity.py"),
        "current_proof_runner": scripts_root
        / "run-step18-proof-cell-qualification.sh",
        "baseline_proof_runner": scripts_root
        / "run-step18-validation-nonregression-baseline.sh",
        "baseline_prepare_runner": scripts_root
        / "prepare-step18-validation-nonregression-baseline.sh",
        "baseline_policy_compat_patch": verifier_path.parent
        / "baseline"
        / "step18_policy_support_baseline_compat.patch",
        "baseline_free_space_profile_patch": verifier_path.parent
        / "baseline"
        / "step18_free_space_profile.patch",
        "baseline_free_space_query_profile_patch": verifier_path.parent
        / "baseline"
        / "step18_free_space_query_profile.patch",
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
    except OSError as error:
        print(f"proof-cell tool snapshot failed: {error}", file=sys.stderr)
        return 2
    tool_hashes = {entry["role"]: entry["sha256"] for entry in tool_manifest}
    verifier_script_sha256 = tool_hashes["proof_verifier"]
    repository_root = verifier_path.parents[2]
    try:
        summary_destination = resolve_summary_destination(
            args.summary_out, records, repository_root
        )
    except ValueError as error:
        print(f"proof-cell output rejected: {error}", file=sys.stderr)
        return 2

    checks.require(
        "baseline_clean_manifest",
        isinstance(clean_manifest, dict)
        and clean_manifest.get("schema_version") == CLEAN_SCHEMA
        and clean_manifest.get("captured_before_patch") is True
        and source_identity_object(
            nested(
                clean_manifest,
                "repositories",
                "manyforge",
                "source_identity",
            )
        )
        and source_identity_object(
            nested(clean_manifest, "repositories", "specs", "source_identity")
        )
        and specs_contract_identity_object(
            nested(
                clean_manifest,
                "repositories",
                "specs",
                "specs_contract_identity",
            )
        )
        and nested(
            clean_manifest,
            "repositories",
            "manyforge",
            "source_identity",
            "head_commit",
        )
        == BASELINE_SOURCE
        and nested(
            clean_manifest,
            "repositories",
            "manyforge",
            "source_identity",
            "dirty",
        )
        is False
        and nested(
            clean_manifest,
            "repositories",
            "specs",
            "source_identity",
            "head_commit",
        )
        == BASELINE_SPECS
        and nested(
            clean_manifest,
            "repositories",
            "specs",
            "source_identity",
            "dirty",
        )
        is False
        and nested(
            clean_manifest,
            "repositories",
            "specs",
            "specs_contract_identity",
            "head_commit",
        )
        == BASELINE_SPECS
        and nested(
            clean_manifest,
            "repositories",
            "specs",
            "specs_contract_identity",
            "head_tree",
        )
        == nested(
            clean_manifest,
            "repositories",
            "specs",
            "source_identity",
            "head_tree",
        )
        and nested(
            clean_manifest,
            "repositories",
            "specs",
            "specs_contract_identity",
            "contract_dirty",
        )
        is False,
    )
    checks.require(
        "baseline_qualification_patch_scope",
        qualification_paths
        == {
            "tests/cpp/CMakeLists.txt",
            "tests/cpp/perf/perf_collision_engine_qualification.cpp",
            "tests/cpp/perf/perf_contact_grasp_validation_nonregression.cpp",
            "tests/cpp/perf/perf_policy_support_contact_qualification.cpp",
        },
        f"paths={sorted(qualification_paths)}",
    )

    lightweight: dict[tuple[str, int], list[dict[str, Any]]] = defaultdict(list)
    policy_support: dict[
        tuple[str, str, int, int], list[dict[str, Any]]
    ] = defaultdict(list)
    contact: dict[tuple[str, int, int], list[dict[str, Any]]] = defaultdict(list)
    free_space: dict[str, list[dict[str, Any]]] = defaultdict(list)
    precise_reference: dict[
        str, list[tuple[dict[str, Any], dict[str, Any]]]
    ] = defaultdict(list)
    for record in records:
        validate_envelope(record, checks)
        cell = record.get("cell")
        if cell == "lightweight_collision_only":
            key = validate_lightweight(record, checks)
            if key is not None:
                lightweight[key].append(record)
        elif cell == "policy_support_contact":
            key = validate_policy_support(record, checks)
            if key is not None:
                policy_support[key].append(record)
        elif cell == "precise_reference_parity":
            precise_result = validate_precise_reference(record, checks)
            if precise_result is not None:
                role, normalized = precise_result
                precise_reference[role].append((record, normalized))
        elif cell == "contact_grasp":
            key = validate_contact(record, checks)
            if key is not None:
                contact[key].append(record)
        elif cell == "free_space":
            role = validate_free_space(record, checks)
            if role is not None:
                free_space[role].append(record)

    required_groups: list[tuple[str, list[dict[str, Any]]]] = []
    if not args.policy_support_only:
        for waypoints in LIGHTWEIGHT_WAYPOINTS:
            for arm in ("collision_only", "binary_contact"):
                required_groups.append(
                    (
                        f"lightweight:{arm}:{waypoints}",
                        lightweight[(arm, waypoints)],
                    )
                )
        for waypoints in WAYPOINTS:
            for workers in CONTACT_WORKER_PROFILES:
                for role in (BASELINE_ROLE, CURRENT_ROLE):
                    required_groups.append(
                        (
                            f"contact_grasp:{role}:{waypoints}:workers={workers}",
                            contact[(role, waypoints, workers)],
                        )
                    )
        for role in (BASELINE_ROLE, CURRENT_ROLE):
            required_groups.append((f"free_space:{role}", free_space[role]))
        for role in (BASELINE_ROLE, CURRENT_ROLE):
            required_groups.append(
                (
                    f"precise_reference_parity:{role}",
                    [record for record, _ in precise_reference[role]],
                )
            )
    policy_expected_groups: list[tuple[str, list[dict[str, Any]]]] = []
    for waypoints in POLICY_WAYPOINTS:
        for workers in CONTACT_WORKER_PROFILES:
            policy_expected_groups.append(
                (
                    f"policy_support:{BASELINE_ROLE}:current_all_active_distance:{waypoints}:workers={workers}",
                    policy_support[
                        (
                            BASELINE_ROLE,
                            "current_all_active_distance",
                            waypoints,
                            workers,
                        )
                    ],
                )
            )
            for arm in (
                "signed_penetration_selective",
                "binary_contact",
                "bounded_penetration",
                "collision_only",
            ):
                policy_expected_groups.append(
                    (
                        f"policy_support:{CURRENT_ROLE}:{arm}:{waypoints}:workers={workers}",
                        policy_support[(CURRENT_ROLE, arm, waypoints, workers)],
                    )
                )
    if args.development_smoke:
        required_groups.extend(
            (name, group) for name, group in policy_expected_groups if group
        )
        checks.require(
            "development_smoke_has_baseline_and_current",
            any(
                key[0] == BASELINE_ROLE and group
                for key, group in policy_support.items()
            )
            and any(
                key[0] == CURRENT_ROLE and group
                for key, group in policy_support.items()
            ),
        )
    else:
        required_groups.extend(policy_expected_groups)
    for name, group in required_groups:
        run_ids = {nested(record, "identity", "run_id") for record in group}
        process_ids = {
            nested(record, "identity", "process_instance_id") for record in group
        }
        paths = {nested(record, "_artifact", "path") for record in group}
        artifact_hashes = {
            nested(record, "_artifact", "sha256") for record in group
        }
        checks.require(
            f"group:{name}:independent_processes",
            len(group) == args.minimum_runs
            and len(run_ids) == len(group)
            and len(process_ids) == len(group)
            and len(paths) == len(group)
            and len(artifact_hashes) == len(group),
            f"records={len(group)} runs={len(run_ids)} processes={len(process_ids)} "
            f"paths={len(paths)} hashes={len(artifact_hashes)}",
        )
    if not args.development_smoke:
        checks.require(
            "formal_exact_record_population",
            len(required_groups) == 48
            and len(records) == 144
            and len(artifact_manifest) == 146,
            f"groups={len(required_groups)} records={len(records)} "
            f"manifest_entries={len(artifact_manifest)}",
        )

    all_run_ids = [nested(record, "identity", "run_id") for record in records]
    all_process_ids = [
        nested(record, "identity", "process_instance_id") for record in records
    ]
    all_paths = [nested(record, "_artifact", "path") for record in records]
    all_artifact_hashes = [
        nested(record, "_artifact", "sha256") for record in records
    ]
    checks.require(
        "all_artifact_process_identities_unique",
        len(all_run_ids) == len(set(all_run_ids))
        and len(all_process_ids) == len(set(all_process_ids))
        and len(all_paths) == len(set(all_paths))
        and len(all_artifact_hashes) == len(set(all_artifact_hashes)),
    )
    current_records = [
        record
        for record in records
        if nested(record, "identity", "source_role") == CURRENT_ROLE
    ]
    baseline_records = [
        record
        for record in records
        if nested(record, "identity", "source_role") == BASELINE_ROLE
    ]
    def summary_identity(
        role_records: list[dict[str, Any]], field: str
    ) -> dict[str, Any] | None:
        values = [nested(record, "identity", field) for record in role_records]
        if not values or not all(isinstance(value, dict) for value in values):
            return None
        encoded = {
            json.dumps(value, sort_keys=True, separators=(",", ":"))
            for value in values
        }
        return json.loads(next(iter(encoded))) if len(encoded) == 1 else None

    candidate_source_identity = summary_identity(
        current_records, "source_identity"
    )
    candidate_specs_source_identity = summary_identity(
        current_records, "specs_source_identity"
    )
    candidate_specs_contract_identity = summary_identity(
        current_records, "specs_contract_identity"
    )
    baseline_source_identity = summary_identity(
        baseline_records, "source_identity"
    )
    baseline_specs_source_identity = summary_identity(
        baseline_records, "specs_source_identity"
    )
    baseline_specs_contract_identity = summary_identity(
        baseline_records, "specs_contract_identity"
    )
    current_identity = {
        (
            nested(
                record,
                "identity",
                "source_identity",
                "fingerprint_sha256",
            ),
            nested(
                record,
                "identity",
                "specs_source_identity",
                "fingerprint_sha256",
            ),
            nested(
                record,
                "identity",
                "specs_contract_identity",
                "contract_fingerprint_sha256",
            ),
            nested(record, "identity", "image_id"),
            nested(record, "identity", "requested_cpu_affinity"),
        )
        for record in current_records
    }
    baseline_identity = {
        (
            nested(
                record,
                "identity",
                "source_identity",
                "fingerprint_sha256",
            ),
            nested(
                record,
                "identity",
                "specs_source_identity",
                "fingerprint_sha256",
            ),
            nested(
                record,
                "identity",
                "specs_contract_identity",
                "contract_fingerprint_sha256",
            ),
        )
        for record in baseline_records
    }
    environments = {
        (
            nested(record, "provenance", "runner_environment", "image_id"),
            nested(
                record,
                "provenance",
                "runner_environment",
                "requested_cpu_affinity",
            ),
            nested(record, "provenance", "runner_environment", "compiler"),
            nested(
                record, "provenance", "runner_environment", "compiler_flags"
            ),
            nested(record, "provenance", "runner_environment", "build_type"),
            nested(record, "provenance", "runner_environment", "build_jobs"),
            nested(
                record, "provenance", "runner_environment", "cpu_governor_set"
            ),
            nested(
                record,
                "provenance",
                "runner_environment",
                "cpu_frequency_driver_set",
            ),
            nested(record, "provenance", "runner_environment", "kernel_id"),
        )
        for record in records
    }
    checks.require("current_single_frozen_identity", len(current_identity) == 1)
    checks.require("baseline_single_frozen_identity", len(baseline_identity) == 1)
    checks.require("globally_matched_execution_environment", len(environments) == 1)
    clean_source_identity = nested(
        clean_manifest, "repositories", "manyforge", "source_identity", default={}
    )
    clean_specs_identity = nested(
        clean_manifest, "repositories", "specs", "source_identity", default={}
    )
    clean_specs_contract_identity = nested(
        clean_manifest,
        "repositories",
        "specs",
        "specs_contract_identity",
        default={},
    )
    checks.require(
        "baseline_provenance_binds_inputs",
        bool(baseline_records)
        and all(
            nested(record, "provenance", "clean_checkout_manifest_sha256")
            == clean_manifest_hash
            and nested(record, "provenance", "qualification_patch_sha256")
            == qualification_patch_hash
            and nested(
                record, "identity", "source_identity", "head_commit"
            )
            == clean_source_identity.get("head_commit")
            and nested(record, "identity", "source_identity", "head_tree")
            == clean_source_identity.get("head_tree")
            and nested(record, "identity", "specs_source_identity")
            == clean_specs_identity
            and nested(record, "identity", "specs_contract_identity")
            == clean_specs_contract_identity
            for record in baseline_records
        ),
    )

    policy_records = [
        record for group in policy_support.values() for record in group
    ]
    if args.policy_support_only:
        checks.require(
            "policy_support_only_input_scope",
            len(policy_records) == len(records),
        )
    policy_environment = {
        (
            nested(record, "payload", "effective_cpu_affinity"),
            nested(record, "payload", "scheduler_policy"),
            nested(record, "payload", "scheduler_priority"),
            nested(record, "payload", "process_nice"),
            nested(record, "payload", "kernel_id"),
            nested(record, "payload", "cpu_governor_set"),
            nested(record, "payload", "cpu_frequency_driver_set"),
            nested(record, "payload", "compiler"),
            nested(record, "payload", "build_type"),
            nested(record, "payload", "compiler_flags"),
            nested(record, "identity", "image_id"),
        )
        for record in policy_records
    }
    checks.require(
        "policy_support_matched_actual_environment",
        bool(policy_records)
        and len(policy_environment) == 1
        and all(
            nested(record, "payload", "effective_cpu_affinity")
            == nested(record, "payload", "requested_cpu_affinity")
            for record in policy_records
        ),
    )
    checks.require(
        "policy_support_invariant_policy_id",
        bool(policy_records)
        and {
            nested(record, "payload", "policy_id") for record in policy_records
        }
        == {"support/step18_policy_perf"},
    )
    expected_wrapper_hash = tool_hashes["proof_wrapper"]
    expected_runner_hashes = {
        CURRENT_ROLE: tool_hashes["current_proof_runner"],
        BASELINE_ROLE: tool_hashes["baseline_proof_runner"],
    }
    expected_prepare_hash = tool_hashes["baseline_prepare_runner"]
    expected_compat_hash = tool_hashes["baseline_policy_compat_patch"]
    expected_free_space_profile_hash = tool_hashes[
        "baseline_free_space_profile_patch"
    ]
    expected_free_space_query_profile_hash = tool_hashes[
        "baseline_free_space_query_profile_patch"
    ]
    expected_source_identity_helper_hash = tool_hashes[
        "source_identity_helper"
    ]
    checks.require(
        "proof_wrapper_and_source_identity_helper_pinned",
        bool(records)
        and {
            nested(record, "provenance", "wrapper_script_sha256")
            for record in records
        }
        == {expected_wrapper_hash}
        and {
            nested(record, "provenance", "source_identity_helper_sha256")
            for record in records
        }
        == {expected_source_identity_helper_hash},
    )
    for role in (BASELINE_ROLE, CURRENT_ROLE):
        role_records = [
            record
            for record in records
            if nested(record, "identity", "source_role") == role
        ]
        if role_records:
            checks.require(
                f"proof_runner_script_pinned:{role}",
                {
                    nested(record, "provenance", "runner_script_sha256")
                    for record in role_records
                }
                == {expected_runner_hashes[role]},
            )
    if baseline_records:
        checks.require(
            "baseline_prepare_and_overlay_assets_pinned",
            all(
                nested(record, "provenance", "runner_environment", "prepare_script_sha256")
                == expected_prepare_hash
                and nested(record, "provenance", "runner_environment", "policy_compat_patch_sha256")
                == expected_compat_hash
                and nested(
                    record,
                    "provenance",
                    "runner_environment",
                    "free_space_profile_patch_sha256",
                )
                == expected_free_space_profile_hash
                and nested(
                    record,
                    "provenance",
                    "runner_environment",
                    "free_space_query_profile_patch_sha256",
                )
                == expected_free_space_query_profile_hash
                for record in baseline_records
            ),
        )
    precise_reference_summary: dict[str, Any] | None = None
    if not args.policy_support_only:
        precise_fields = (
            "input_corpus_sha256",
            "normalized_transcript_sha256",
            "normalization_contract_sha256",
            "orchestration_projection_contract_sha256",
            "small_case_profile_sha256",
            "small_orchestration_profile_sha256",
            "small_case_sha256",
            "partition_profile_sha256",
            "partition_orchestration_profile_sha256",
            "completion_order_sha256",
            "completion_orchestration_sha256",
            "exceptional_case_sha256",
        )
        role_representatives: dict[str, dict[str, Any]] = {}
        precise_repetition_stable = True
        per_record_transcripts: list[dict[str, Any]] = []
        for role in (BASELINE_ROLE, CURRENT_ROLE):
            entries = precise_reference[role]
            if len(entries) != 3:
                precise_repetition_stable = False
                continue
            for field in precise_fields:
                if len(
                    {
                        json.dumps(
                            normalized[field],
                            sort_keys=True,
                            separators=(",", ":"),
                        )
                        for _, normalized in entries
                    }
                ) != 1:
                    precise_repetition_stable = False
            audits = {
                json.dumps(
                    normalized["normalization_audit"],
                    sort_keys=True,
                    separators=(",", ":"),
                )
                for _, normalized in entries
            }
            if len(audits) != 1:
                precise_repetition_stable = False
            role_representatives[role] = entries[0][1]
            for repetition, (record, normalized) in enumerate(
                sorted(
                    entries,
                    key=lambda item: nested(item[0], "identity", "run_id"),
                ),
                start=1,
            ):
                per_record_transcripts.append(
                    {
                        "source_role": role,
                        "repetition": repetition,
                        "run_id": nested(record, "identity", "run_id"),
                        "process_instance_id": nested(
                            record, "identity", "process_instance_id"
                        ),
                        "artifact_sha256": nested(
                            record, "_artifact", "sha256"
                        ),
                        "input_corpus_sha256": normalized[
                            "input_corpus_sha256"
                        ],
                        "normalized_transcript_sha256": normalized[
                            "normalized_transcript_sha256"
                        ],
                        "small_case_profile_sha256": normalized[
                            "small_case_profile_sha256"
                        ],
                        "small_case_sha256": normalized[
                            "small_case_sha256"
                        ],
                        "partition_profile_sha256": normalized[
                            "partition_profile_sha256"
                        ],
                        "completion_order_sha256": normalized[
                            "completion_order_sha256"
                        ],
                        "exceptional_case_sha256": normalized[
                            "exceptional_case_sha256"
                        ],
                    }
                )
        checks.require(
            "precise_reference_three_repetitions_stable_per_source_role",
            precise_repetition_stable
            and len(per_record_transcripts) == 6
            and len(
                {
                    item["run_id"] for item in per_record_transcripts
                }
            )
            == 6
            and len(
                {
                    item["process_instance_id"]
                    for item in per_record_transcripts
                }
            )
            == 6,
        )
        baseline_precise = role_representatives.get(BASELINE_ROLE)
        current_precise = role_representatives.get(CURRENT_ROLE)
        normalized_parity = (
            baseline_precise is not None
            and current_precise is not None
            and all(
                baseline_precise[field] == current_precise[field]
                for field in precise_fields
            )
        )
        checks.require(
            "precise_reference_matched_profile_full_stable_parity",
            normalized_parity,
        )
        baseline_audit = (
            baseline_precise.get("normalization_audit", {})
            if baseline_precise
            else {}
        )
        current_audit = (
            current_precise.get("normalization_audit", {})
            if current_precise
            else {}
        )
        normalization_audit_exact = (
            baseline_audit.get("baseline_legacy_zero_observations", 0) > 0
            and baseline_audit.get("baseline_legacy_zero_observations")
            == baseline_audit.get(
                "baseline_implied_signed_mode_observations"
            )
            and baseline_audit.get("current_explicit_signed_mode_observations")
            == 0
            and baseline_audit.get("baseline_raw_mode_absent_profiles") == 50
            and baseline_audit.get("current_raw_mode_present_profiles") == 0
            and baseline_audit.get(
                "baseline_missing_metric_code3_observations"
            )
            == 5
            and baseline_audit.get(
                "current_missing_metric_code8_observations"
            )
            == 0
            and current_audit.get("baseline_legacy_zero_observations") == 0
            and current_audit.get(
                "baseline_implied_signed_mode_observations"
            )
            == 0
            and current_audit.get(
                "current_explicit_signed_mode_observations", 0
            )
            > 0
            and current_audit.get("baseline_raw_mode_absent_profiles") == 0
            and current_audit.get("current_raw_mode_present_profiles") == 50
            and current_audit.get(
                "baseline_missing_metric_code3_observations"
            )
            == 0
            and current_audit.get(
                "current_missing_metric_code8_observations"
            )
            == 5
            and baseline_audit.get(
                "omitted_global_volatile_diagnostic_values"
            )
            == 0
            and current_audit.get(
                "omitted_global_volatile_diagnostic_values"
            )
            == 0
        )
        checks.require(
            "precise_reference_closed_normalization_audit",
            normalization_audit_exact,
            f"baseline={baseline_audit} current={current_audit}",
        )
        representative = current_precise or baseline_precise or {}
        precise_reference_summary = {
            "timing_gate_excluded": True,
            "record_count": len(per_record_transcripts),
            "runs_per_source_role": 3,
            "case_count": len(PRECISE_REFERENCE_CASE_ORDER),
            "case_order": list(PRECISE_REFERENCE_CASE_ORDER),
            "small_case_worker_counts": list(PRECISE_REFERENCE_WORKERS),
            "normalization_contract_id": PRECISE_REFERENCE_NORMALIZATION_ID,
            "normalization_contract_sha256": representative.get(
                "normalization_contract_sha256"
            ),
            "orchestration_projection_contract_id": (
                "step18_cross_worker_orchestration_projection.v1"
            ),
            "orchestration_projection_contract_sha256": representative.get(
                "orchestration_projection_contract_sha256"
            ),
            "input_corpus_sha256": representative.get(
                "input_corpus_sha256"
            ),
            "baseline_transcript_sha256": (
                baseline_precise.get("normalized_transcript_sha256")
                if baseline_precise
                else None
            ),
            "current_transcript_sha256": (
                current_precise.get("normalized_transcript_sha256")
                if current_precise
                else None
            ),
            "semantic_parity": normalized_parity
            and normalization_audit_exact
            and precise_repetition_stable,
            "matched_profile_full_stable_parity": normalized_parity,
            "cross_worker_orchestration_projection": {
                "excluded_result_fields": ["query_id"],
                "small_case_profile_sha256": representative.get(
                    "small_orchestration_profile_sha256"
                ),
                "partition_profile_sha256": representative.get(
                    "partition_orchestration_profile_sha256"
                ),
                "completion_sha256": representative.get(
                    "completion_orchestration_sha256"
                ),
                "parity": normalized_parity,
            },
            "per_record_transcripts": per_record_transcripts,
            "normalization_audit": {
                BASELINE_ROLE: baseline_audit,
                CURRENT_ROLE: current_audit,
            },
            "partition_inventory": {
                "sample_kind_count": 4,
                "waypoint_count": 5,
                "patterns_per_worker": 1024,
                "worker_counts": list(
                    PRECISE_REFERENCE_PARTITION_WORKERS
                ),
                "total_partition_evaluations_per_record": 4096,
                "serial_reference_evaluations_per_record": 1024,
                "matched_profile_sha256": representative.get(
                    "partition_profile_sha256"
                ),
                "parity": normalized_parity,
            },
            "completion_order_inventory": {
                "workers": 3,
                "permutation_count": 6,
                "orders": [
                    list(order)
                    for order in PRECISE_REFERENCE_COMPLETION_ORDERS
                ],
                "matched_profile_sha256": representative.get(
                    "completion_order_sha256"
                ),
                "parity": normalized_parity,
            },
            "exceptional_cases": {
                "pre_cancel": {
                    "reason": "operator_stop",
                    "status": "not_invoked",
                    "status_code": None,
                    "earliest": {"waypoint": 0, "time_ns": 0},
                    "parity": normalized_parity,
                },
                "expired_deadline": {
                    "reason": "collision_validation_wall_timeout",
                    "status": "deadline_exceeded",
                    "status_code": 1,
                    "earliest": {"waypoint": 0, "time_ns": None},
                    "parity": normalized_parity,
                },
                "worker_exception": {
                    "reason": "collision_validation_worker_exception",
                    "status": "engine_error",
                    "status_code": 5,
                    "earliest": {
                        "waypoint": 4,
                        "time_ns": 40_000_000,
                    },
                    "parity": normalized_parity,
                },
            },
        }
    policy_performance: dict[str, Any] = {}
    for waypoints in POLICY_WAYPOINTS:
        policy_performance[str(waypoints)] = {}
        for workers in CONTACT_WORKER_PROFILES:
            baseline = policy_support[
                (
                    BASELINE_ROLE,
                    "current_all_active_distance",
                    waypoints,
                    workers,
                )
            ]
            arms = {
                arm: policy_support[(CURRENT_ROLE, arm, waypoints, workers)]
                for arm in (
                    "signed_penetration_selective",
                    "binary_contact",
                    "bounded_penetration",
                    "collision_only",
                )
            }
            available = baseline + [
                record for group in arms.values() for record in group
            ]
            if not available:
                continue
            invariant_input_fields = (
                "fixture_id",
                "fixture_variant",
                "fixture_world_profile",
                "corpus_sha256",
                "snapshot_content_hash",
                "snapshot_geometry_fingerprint",
                "state_fingerprint",
                "trajectory_fingerprint",
                "configured_pair_fingerprint",
                "physical_collision_pair_fingerprint",
                "body_count",
                "geometry_count",
                "distractor_count",
                "configured_contact_pair_count",
                "physical_collision_pair_count",
                "incidental_allowed_pair_count",
            )
            input_tuples = {
                tuple(nested(record, "payload", field) for field in invariant_input_fields)
                for record in available
            }
            physical_fields = (
                "decision_accepted",
                "decision_action_code",
                "decision_severity_code",
                "every_waypoint_in_collision",
                "waypoint_record_count",
                "normalized_pair_observation_count",
                "normalized_pair_output_fingerprint",
            )
            physical_tuples = {
                tuple(nested(record, "payload", field) for field in physical_fields)
                for record in available
            }
            checks.require(
                f"policy_support_same_geometry_input:{waypoints}:workers={workers}",
                len(input_tuples) == 1,
            )
            checks.require(
                f"policy_support_full_waypoint_physical_parity:{waypoints}:workers={workers}",
                len(physical_tuples) == 1,
            )
            mechanism_parity_fields = (
                "waypoint_count",
                "mechanism_query_count",
                "collision_api_call_count",
                "active_collision_pair_count",
                "colliding_geometry_pair_count",
                "colliding_logical_pair_count",
            )
            mechanism_tuples = {
                tuple(
                    nested(record, "payload", field)
                    for field in mechanism_parity_fields
                )
                for record in available
            }
            checks.require(
                f"policy_support_full_collision_enumeration_parity:{waypoints}:workers={workers}",
                len(mechanism_tuples) == 1,
            )
            policy_hash_groups = {
                (nested(record, "identity", "source_role"), nested(record, "payload", "arm")): {
                    nested(record, "payload", "policy_content_hash")
                    for record in available
                    if nested(record, "identity", "source_role") == role
                    and nested(record, "payload", "arm") == group_arm
                }
                for role, group_arm in {
                    (nested(record, "identity", "source_role"), nested(record, "payload", "arm"))
                    for record in available
                }
            }
            checks.require(
                f"policy_support_policy_hash_stable:{waypoints}:workers={workers}",
                all(len(values) == 1 for values in policy_hash_groups.values()),
            )
            current_policy_hashes = {
                nested(record, "payload", "policy_content_hash")
                for arm_group in arms.values()
                for record in arm_group
            }
            if all(arms.values()):
                checks.require(
                    f"policy_support_current_modality_hashes_distinct:{waypoints}:workers={workers}",
                    len(current_policy_hashes) == 4,
                )
            if not baseline:
                continue
            baseline_cpu = median(
                nested(record, "payload", "process_cpu_us", "p95")
                for record in baseline
            )
            baseline_wall = median(
                nested(record, "payload", "wall_us", "p95")
                for record in baseline
            )
            baseline_distance = median(
                nested(record, "payload", "distance_evaluated_logical_pair_count")
                for record in baseline
            )
            cell_result: dict[str, Any] = {
                "qualification_status": (
                    "report_only" if waypoints == 1000 else "gated"
                ),
                "baseline_process_cpu_p95_us": baseline_cpu,
                "baseline_wall_p95_us": baseline_wall,
                "baseline_distance_evaluated_logical_pair_count": baseline_distance,
                "arms": {},
            }
            for arm, group in arms.items():
                if not group:
                    continue
                current_cpu = median(
                    nested(record, "payload", "process_cpu_us", "p95")
                    for record in group
                )
                current_wall = median(
                    nested(record, "payload", "wall_us", "p95")
                    for record in group
                )
                current_distance = median(
                    nested(record, "payload", "distance_evaluated_logical_pair_count")
                    for record in group
                )
                cpu_ratio = current_cpu / baseline_cpu if baseline_cpu > 0 else math.nan
                wall_ratio = current_wall / baseline_wall if baseline_wall > 0 else math.nan
                reduction = (
                    1.0 - current_distance / baseline_distance
                    if baseline_distance > 0
                    else math.nan
                )
                cell_result["arms"][arm] = {
                    "process_cpu_p95_us": current_cpu,
                    "wall_p95_us": current_wall,
                    "process_cpu_p95_ratio_to_baseline": cpu_ratio,
                    "wall_p95_ratio_to_baseline": wall_ratio,
                    "distance_evaluated_logical_pair_count": current_distance,
                    "distance_evaluation_reduction": reduction,
                }
                checks.require(
                    f"policy_support_distance_evaluation_reduction:{arm}:{waypoints}:workers={workers}",
                    math.isfinite(reduction)
                    and reduction >= POLICY_DISTANCE_REDUCTION_MINIMUM,
                    f"reduction={reduction:.6f} minimum={POLICY_DISTANCE_REDUCTION_MINIMUM}",
                )
                if waypoints in (96, 500) and arm in {
                    "signed_penetration_selective",
                    "bounded_penetration",
                }:
                    checks.require(
                        f"policy_support_distance_cpu_ratio:{arm}:{waypoints}:workers={workers}",
                        math.isfinite(cpu_ratio)
                        and cpu_ratio <= POLICY_DISTANCE_CPU_RATIO_LIMIT,
                        f"ratio={cpu_ratio:.6f} limit={POLICY_DISTANCE_CPU_RATIO_LIMIT}",
                    )
                if waypoints in (96, 500) and arm == "binary_contact":
                    checks.require(
                        f"policy_support_binary_cpu_ratio:{waypoints}:workers={workers}",
                        math.isfinite(cpu_ratio)
                        and cpu_ratio <= POLICY_BINARY_CPU_RATIO_LIMIT,
                        f"ratio={cpu_ratio:.6f} limit={POLICY_BINARY_CPU_RATIO_LIMIT}",
                    )
                if waypoints in (96, 500):
                    checks.require(
                        f"policy_support_wall_ratio:{arm}:{waypoints}:workers={workers}",
                        math.isfinite(wall_ratio)
                        and wall_ratio <= NONREGRESSION_RATIO_LIMIT,
                        f"ratio={wall_ratio:.6f} limit={NONREGRESSION_RATIO_LIMIT}",
                    )
            binary = arms["binary_contact"]
            comparator = arms["collision_only"]
            if binary and comparator:
                binary_cpu = median(
                    nested(record, "payload", "process_cpu_us", "p95")
                    for record in binary
                )
                comparator_cpu = median(
                    nested(record, "payload", "process_cpu_us", "p95")
                    for record in comparator
                )
                ratio = (
                    binary_cpu / comparator_cpu
                    if comparator_cpu > 0
                    else math.nan
                )
                cell_result["binary_to_collision_only_process_cpu_p95_ratio"] = ratio
                checks.require(
                    f"policy_support_binary_to_collision_only_cpu_ratio:{waypoints}:workers={workers}",
                    math.isfinite(ratio) and ratio <= LIGHTWEIGHT_RATIO_LIMIT,
                    f"ratio={ratio:.6f} limit={LIGHTWEIGHT_RATIO_LIMIT}",
                )
            policy_performance[str(waypoints)][str(workers)] = cell_result

    if args.policy_support_only:
        require_current_candidate_checkout(records, checks, repository_root)
        require_artifacts_unchanged(artifact_manifest, checks)
        require_artifacts_unchanged(
            tool_manifest, checks, "retained_tool_unchanged"
        )
        failures = checks.failures
        summary = {
            "schema_version": SUMMARY_SCHEMA,
            "verification_scope": "policy_support_development_smoke",
            "minimum_runs": args.minimum_runs,
            "development_smoke": args.development_smoke,
            "offline_proof_gate_passed": False,
            "overall_step18_closure_claimed": False,
            "requires_common_candidate_aggregator": True,
            "record_count": len(records),
            "artifact_manifest": artifact_manifest,
            "tool_manifest": tool_manifest,
            "candidate_source_identity": candidate_source_identity,
            "candidate_specs_source_identity": candidate_specs_source_identity,
            "candidate_specs_contract_identity": (
                candidate_specs_contract_identity
            ),
            "baseline_source_identity": baseline_source_identity,
            "baseline_specs_source_identity": baseline_specs_source_identity,
            "baseline_specs_contract_identity": (
                baseline_specs_contract_identity
            ),
            "baseline_clean_checkout_manifest_sha256": clean_manifest_hash,
            "baseline_qualification_patch_sha256": qualification_patch_hash,
            "verifier_script_sha256": verifier_script_sha256,
            "performance": {"policy_support_contact": policy_performance},
            "checks": checks.items,
            "failure_count": len(failures),
            "development_smoke_passed": not failures,
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
                    tool_manifest,
                )
            except (OSError, ValueError) as error:
                print(f"proof-cell summary write failed: {error}", file=sys.stderr)
                return 2
        return 0 if not failures else 1

    performance: dict[str, Any] = {
        "lightweight_collision_only": {},
        "policy_support_contact": policy_performance,
        "contact_grasp": {},
        "free_space": {},
    }
    for waypoints in LIGHTWEIGHT_WAYPOINTS:
        binary = lightweight[("binary_contact", waypoints)]
        collision_only = lightweight[("collision_only", waypoints)]
        binary_p95 = median(
            nested(record, "payload", "cpu_time_ns", "p95") for record in binary
        )
        collision_only_p95 = median(
            nested(record, "payload", "cpu_time_ns", "p95")
            for record in collision_only
        )
        ratio = binary_p95 / collision_only_p95 if collision_only_p95 > 0 else math.nan
        performance["lightweight_collision_only"][str(waypoints)] = {
            "binary_contact_process_cpu_p95_ns": binary_p95,
            "collision_only_process_cpu_p95_ns": collision_only_p95,
            "lightweight_to_collision_only_process_cpu_p95_ratio": ratio,
            "limit": LIGHTWEIGHT_RATIO_LIMIT,
        }
        checks.require(
            f"lightweight_to_collision_only_process_cpu_p95_ratio:{waypoints}",
            math.isfinite(ratio) and ratio <= LIGHTWEIGHT_RATIO_LIMIT,
            f"ratio={ratio:.6f} limit={LIGHTWEIGHT_RATIO_LIMIT}",
        )
        for left in binary:
            for right in collision_only:
                left_value = left.get("payload", {})
                right_value = right.get("payload", {})
                physical_output_fields = (
                    "physical_output_in_collision",
                    "physical_output_engine_status_code",
                    "physical_output_decision_code",
                    "physical_output_normalized_pair_count",
                    "physical_output_normalized_pair_fingerprint",
                )
                mechanism_fields = (
                    "engine_id",
                    "engine_version",
                    "body_count",
                    "geometry_count",
                    "contains_mesh_geometry",
                    "normalized_pair_count",
                    "collision_api_call_count",
                    "collision_enumeration_completion_ordinal",
                    "active_collision_pair_count",
                    "colliding_geometry_pair_count",
                    "colliding_logical_pair_count",
                    "fk_placement_invocation_count",
                )
                checks.require(
                    f"lightweight_collision_only_physical_output_parity:{waypoints}:{record_label(left)}:{record_label(right)}",
                    all(
                        left_value.get(field) == right_value.get(field)
                        for field in physical_output_fields
                    ),
                )
                checks.require(
                    f"lightweight_collision_only_mechanism_parity:{waypoints}:{record_label(left)}:{record_label(right)}",
                    all(
                        left_value.get(field) == right_value.get(field)
                        for field in mechanism_fields
                    ),
                )

    for waypoints in WAYPOINTS:
        performance["contact_grasp"][str(waypoints)] = {}
        for workers in CONTACT_WORKER_PROFILES:
            baseline = contact[(BASELINE_ROLE, waypoints, workers)]
            current = contact[(CURRENT_ROLE, waypoints, workers)]
            baseline_wall = median(
                nested(record, "payload", "wall_us", "p95")
                for record in baseline
            )
            current_wall = median(
                nested(record, "payload", "wall_us", "p95")
                for record in current
            )
            baseline_cpu = median(
                nested(record, "payload", "process_cpu_us", "p95")
                for record in baseline
            )
            current_cpu = median(
                nested(record, "payload", "process_cpu_us", "p95")
                for record in current
            )
            wall_ratio = (
                current_wall / baseline_wall
                if baseline_wall > 0
                else math.nan
            )
            cpu_ratio = (
                current_cpu / baseline_cpu
                if baseline_cpu > 0
                else math.nan
            )
            performance["contact_grasp"][str(waypoints)][str(workers)] = {
                "baseline_wall_p95_us": baseline_wall,
                "current_wall_p95_us": current_wall,
                "contact_grasp_wall_p95_ratio": wall_ratio,
                "baseline_process_cpu_p95_us": baseline_cpu,
                "current_process_cpu_p95_us": current_cpu,
                "contact_grasp_process_cpu_p95_ratio": cpu_ratio,
                "limit": NONREGRESSION_RATIO_LIMIT,
            }
            checks.require(
                f"contact_grasp_wall_p95_ratio:{waypoints}:workers={workers}",
                math.isfinite(wall_ratio)
                and wall_ratio <= NONREGRESSION_RATIO_LIMIT,
                f"ratio={wall_ratio:.6f} limit={NONREGRESSION_RATIO_LIMIT}",
            )
            checks.require(
                f"contact_grasp_process_cpu_p95_ratio:{waypoints}:workers={workers}",
                math.isfinite(cpu_ratio)
                and cpu_ratio <= NONREGRESSION_RATIO_LIMIT,
                f"ratio={cpu_ratio:.6f} limit={NONREGRESSION_RATIO_LIMIT}",
            )
            baseline_outputs = {
                (
                    nested(record, "payload", "decision_accepted"),
                    nested(record, "payload", "initial_waypoint_clear"),
                    nested(
                        record,
                        "payload",
                        "every_contact_waypoint_in_collision",
                    ),
                    nested(record, "payload", "waypoint_record_count"),
                    nested(
                        record,
                        "payload",
                        "normalized_pair_observation_count",
                    ),
                    nested(
                        record,
                        "payload",
                        "finite_colliding_clearance_observation_count",
                    ),
                    nested(
                        record,
                        "payload",
                        "normalized_pair_output_fingerprint",
                    ),
                )
                for record in baseline
            }
            current_outputs = {
                (
                    nested(record, "payload", "decision_accepted"),
                    nested(record, "payload", "initial_waypoint_clear"),
                    nested(
                        record,
                        "payload",
                        "every_contact_waypoint_in_collision",
                    ),
                    nested(record, "payload", "waypoint_record_count"),
                    nested(
                        record,
                        "payload",
                        "normalized_pair_observation_count",
                    ),
                    nested(
                        record,
                        "payload",
                        "finite_colliding_clearance_observation_count",
                    ),
                    nested(
                        record,
                        "payload",
                        "normalized_pair_output_fingerprint",
                    ),
                )
                for record in current
            }
            baseline_clearance_fingerprints = {
                nested(
                    record,
                    "payload",
                    "colliding_clearance_output_fingerprint",
                )
                for record in baseline
            }
            current_clearance_fingerprints = {
                nested(
                    record,
                    "payload",
                    "colliding_clearance_output_fingerprint",
                )
                for record in current
            }
            checks.require(
                f"contact_grasp_decision_physical_pair_parity:{waypoints}:workers={workers}",
                len(baseline_outputs) == 1
                and baseline_outputs == current_outputs,
            )
            checks.require(
                f"contact_grasp_clearance_fingerprint_stable_per_source:{waypoints}:workers={workers}",
                len(baseline_clearance_fingerprints) == 1
                and len(current_clearance_fingerprints) == 1,
            )
            performance["contact_grasp"][str(waypoints)][str(workers)][
                "physical_output"
            ] = {
                "normalized_pair_output_fingerprint": next(
                    iter(baseline_outputs), (None,) * 7
                )[-1],
                "baseline_colliding_clearance_output_fingerprint": next(
                    iter(baseline_clearance_fingerprints), None
                ),
                "current_colliding_clearance_output_fingerprint": next(
                    iter(current_clearance_fingerprints), None
                ),
            }

    baseline_free = free_space[BASELINE_ROLE]
    current_free = free_space[CURRENT_ROLE]
    for fixture_name in BASELINE_FREE_SPACE_FIXTURES:
        baseline_fixture_records = [
            fixture_map(record.get("payload", {})).get(fixture_name, {})
            for record in baseline_free
        ]
        current_fixture_records = [
            fixture_map(record.get("payload", {})).get(fixture_name, {})
            for record in current_free
        ]
        baseline_keys = set.intersection(
            *(set(worker_map(fixture)) for fixture in baseline_fixture_records)
        ) if baseline_fixture_records else set()
        current_keys = set.intersection(
            *(set(worker_map(fixture)) for fixture in current_fixture_records)
        ) if current_fixture_records else set()
        checks.require(
            f"free_space_matched_worker_keys:{fixture_name}",
            baseline_keys == current_keys and len(baseline_keys) == 4,
        )
        performance["free_space"][fixture_name] = {}
        for key in sorted(baseline_keys & current_keys):
            baseline_wall = median(
                nested(worker_map(fixture)[key], "wall_us", "p95")
                for fixture in baseline_fixture_records
            )
            current_wall = median(
                nested(worker_map(fixture)[key], "wall_us", "p95")
                for fixture in current_fixture_records
            )
            baseline_cpu = median(
                nested(worker_map(fixture)[key], "process_cpu_us", "p95")
                for fixture in baseline_fixture_records
            )
            current_cpu = median(
                nested(worker_map(fixture)[key], "process_cpu_us", "p95")
                for fixture in current_fixture_records
            )
            wall_ratio = current_wall / baseline_wall if baseline_wall > 0 else math.nan
            cpu_ratio = current_cpu / baseline_cpu if baseline_cpu > 0 else math.nan
            worker_label = str(key[0])
            performance["free_space"][fixture_name][worker_label] = {
                "wall_p95_ratio": wall_ratio,
                "process_cpu_p95_ratio": cpu_ratio,
                "limit": NONREGRESSION_RATIO_LIMIT,
            }
            checks.require(
                f"free_space_wall_p95_ratio:{fixture_name}:workers={key[0]}",
                math.isfinite(wall_ratio) and wall_ratio <= NONREGRESSION_RATIO_LIMIT,
                f"ratio={wall_ratio:.6f} limit={NONREGRESSION_RATIO_LIMIT}",
            )
            checks.require(
                f"free_space_process_cpu_p95_ratio:{fixture_name}:workers={key[0]}",
                math.isfinite(cpu_ratio) and cpu_ratio <= NONREGRESSION_RATIO_LIMIT,
                f"ratio={cpu_ratio:.6f} limit={NONREGRESSION_RATIO_LIMIT}",
            )
        baseline_decisions = {
            tuple(
                fixture.get(field)
                for field in (
                    "manyforge_collision_status",
                    "manyforge_collision_decision",
                    "manyforge_in_collision",
                    "roboplan_shadow_status",
                    "roboplan_shadow_decision",
                    "roboplan_in_collision",
                )
            )
            for fixture in baseline_fixture_records
        }
        current_decisions = {
            tuple(
                fixture.get(field)
                for field in (
                    "manyforge_collision_status",
                    "manyforge_collision_decision",
                    "manyforge_in_collision",
                    "roboplan_shadow_status",
                    "roboplan_shadow_decision",
                    "roboplan_in_collision",
                )
            )
            for fixture in current_fixture_records
        }
        checks.require(
            f"free_space_decision_parity:{fixture_name}",
            len(baseline_decisions) == 1 and baseline_decisions == current_decisions,
        )

    require_current_candidate_checkout(records, checks, repository_root)
    require_artifacts_unchanged(artifact_manifest, checks)
    require_artifacts_unchanged(tool_manifest, checks, "retained_tool_unchanged")
    failures = checks.failures
    summary = {
        "schema_version": SUMMARY_SCHEMA,
        "verification_scope": "offline_proof_cells",
        "minimum_runs": args.minimum_runs,
        "development_smoke": False,
        "offline_proof_gate_passed": not failures,
        "overall_step18_closure_claimed": False,
        "requires_common_candidate_aggregator": True,
        "record_count": len(records),
        "artifact_manifest": artifact_manifest,
        "tool_manifest": tool_manifest,
        "candidate_source_identity": candidate_source_identity,
        "candidate_specs_source_identity": candidate_specs_source_identity,
        "candidate_specs_contract_identity": candidate_specs_contract_identity,
        "baseline_source_identity": baseline_source_identity,
        "baseline_specs_source_identity": baseline_specs_source_identity,
        "baseline_specs_contract_identity": baseline_specs_contract_identity,
        "baseline_clean_checkout_manifest_sha256": clean_manifest_hash,
        "baseline_qualification_patch_sha256": qualification_patch_hash,
        "verifier_script_sha256": verifier_script_sha256,
        "precise_reference_parity": precise_reference_summary,
        "performance": performance,
        "checks": checks.items,
        "failure_count": len(failures),
        "verification_passed": not failures,
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
                tool_manifest,
            )
        except (OSError, ValueError) as error:
            print(f"proof-cell summary write failed: {error}", file=sys.stderr)
            return 2
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
