#!/usr/bin/env python3
"""Verify every Step 18 subgate against one immutable current candidate."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable

from generate_step18_closure_manifest import (
    EVIDENCE_SCHEMA,
    INPUTS,
    SCHEMA as MANIFEST_SCHEMA,
    FileTracker,
    ManifestError,
    build_producer_invocations,
    ensure_ignored_output,
    parse_evidence_record,
)
from source_identity import (
    SPECS_CONTRACT_EXCLUDED_PATH,
    SourceIdentityError,
    source_identity,
    specs_contract_identity,
    validate_identity,
    validate_specs_contract_identity,
)


SUMMARY_SCHEMA = "manyforge.step18.closure_verification.v2"
PROOF_SCHEMA = "manyforge.step18.proof_cell_verification.v2"
RUNTIME_SCHEMA = "manyforge.runtime_collision_schedule_verification.v2"
LIVE_SCHEMA = "manyforge.step18_live_contact_modes.v3"
LOADED_SCHEMA = "manyforge.step18.loaded_kernel_verification.v1"
RELEASE_SCHEMA = "manyforge.step18.release_gate_verification.v1"
PINNED_BASELINE_COMMIT = "643fe9c399182419d38d4478750d7e6f692dc9ad"
PINNED_BASELINE_TREE = "c900d971cdcbcfb6ec5d7f9b465ff18befa1dd89"
PINNED_SPECS_COMMIT = "5e50a4872bbb9a2167c6bc14b9b5963061f2f09e"
REQUIRED_LOADED_PREDICATE_TESTS = frozenset(
    {
        "ExecutionKernelTest.Cancel_StopDuringValidationRequestsValidationCancel",
        "ExecutionKernelTest.Cancel_ResetDuringValidationRequestsValidationCancel",
        "ExecutionKernelTest.Stop_OperatorProfileRefused_EmergencyLegacyFallback",
        "ExecutionKernelTest.Stop_OperatorDeadlineWatchdogHardStopsBeforeBackendReturns",
    }
)
PINNED_SPECS_TREE = "7529497b02758c14666f85bc9bf98e85abec075a"
RELEASE_LANES = {
    "full_dev_regression",
    "full_ros_regression",
    "contract_architecture_hygiene",
    "core_asan_ubsan",
    "core_tsan",
    "collision_asan_ubsan",
    "collision_tsan",
}
SHA256 = re.compile(r"[0-9a-f]{64}")
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
PRECISE_REFERENCE_COMPLETION_ORDERS = (
    (0, 1, 2),
    (0, 2, 1),
    (1, 0, 2),
    (1, 2, 0),
    (2, 0, 1),
    (2, 1, 0),
)


class ValidationError(RuntimeError):
    """The evidence set does not prove complete Step 18 closure."""


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


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"{label} is not readable JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValidationError(f"{label} must contain one JSON object")
    return value


def _json_payload(payload: bytes, label: str) -> dict[str, Any]:
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"{label} is not readable JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValidationError(f"{label} must contain one JSON object")
    return value


def _exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    actual = set(value)
    if actual != expected:
        raise ValidationError(
            f"{label} fields differ: missing={sorted(expected - actual)} "
            f"unexpected={sorted(actual - expected)}"
        )


def _identity(value: Any, label: str) -> dict[str, Any]:
    try:
        return validate_identity(value)
    except SourceIdentityError as error:
        raise ValidationError(f"{label} is invalid: {error}") from error


def _contract_identity(value: Any, label: str) -> dict[str, Any]:
    try:
        return validate_specs_contract_identity(value)
    except SourceIdentityError as error:
        raise ValidationError(f"{label} is invalid: {error}") from error


def _resolve_hashed_file(
    owner_path: Path, binding: Any, label: str, tracker: FileTracker
) -> tuple[Path, str, bytes]:
    if not isinstance(binding, dict) or set(binding) != {"path", "sha256"}:
        raise ValidationError(f"{label} must be an exact path/SHA-256 binding")
    raw_path = binding.get("path")
    if not isinstance(raw_path, str) or not raw_path:
        raise ValidationError(f"{label} path must be non-empty")
    path = Path(raw_path)
    path = path if path.is_absolute() else owner_path.parent / path
    path = path.resolve()
    try:
        payload, digest = tracker.read(path, label)
    except ManifestError as error:
        raise ValidationError(str(error)) from error
    if digest != _sha256_value(binding.get("sha256"), f"{label} hash"):
        raise ValidationError(f"{label} hash changed")
    return path, digest, payload


def _binding_key(
    binding: Any, owner_path: Path, label: str
) -> tuple[str, str]:
    if not isinstance(binding, dict):
        raise ValidationError(f"{label} must be an object")
    raw_path = binding.get("path")
    if not isinstance(raw_path, str) or not raw_path:
        raise ValidationError(f"{label} path must be non-empty")
    path = Path(raw_path)
    path = path if path.is_absolute() else owner_path.parent / path
    return str(path.resolve()), _sha256_value(binding.get("sha256"), f"{label} hash")


def _typed_manifest(
    *,
    raw: Any,
    owner_path: Path,
    label: str,
    count: int,
    tracker: FileTracker,
    exact_fields_for_role: Callable[[str], set[str]],
) -> list[dict[str, Any]]:
    if not isinstance(raw, list) or len(raw) != count:
        raise ValidationError(f"{label} must contain exactly {count} entries")
    normalized: list[dict[str, Any]] = []
    paths: list[Path] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise ValidationError(f"{label}[{index}] is not an object")
        role = item.get("role")
        if not isinstance(role, str) or not role:
            raise ValidationError(f"{label}[{index}] has no typed role")
        _exact_keys(
            item,
            exact_fields_for_role(role),
            f"{label}[{index}]",
        )
        path, digest, _ = _resolve_hashed_file(
            owner_path,
            {"path": item.get("path"), "sha256": item.get("sha256")},
            f"{label}[{index}]",
            tracker,
        )
        paths.append(path)
        normalized.append({"role": role, "path": str(path), "sha256": digest})
    if len(set(paths)) != len(paths):
        raise ValidationError(f"{label} paths are not globally unique")
    return normalized


def _roles_exact(
    entries: list[dict[str, Any]], expected: dict[str, int], label: str
) -> None:
    observed = [entry["role"] for entry in entries]
    if set(observed) != set(expected) or any(
        observed.count(role) != count for role, count in expected.items()
    ):
        raise ValidationError(f"{label} roles/counts are not exact")


def _require_all_summary_bindings_typed(
    summary: dict[str, Any],
    owner_path: Path,
    label: str,
    typed: list[dict[str, Any]],
) -> None:
    allowed = {(entry["path"], entry["sha256"]) for entry in typed}
    untyped: list[str] = []

    def visit(node: Any, route: str) -> None:
        if isinstance(node, dict):
            if "path" in node and "sha256" in node:
                key = _binding_key(node, owner_path, f"{label}.{route}")
                if key not in allowed:
                    untyped.append(f"{route} -> {key[0]}")
            for key, child in node.items():
                visit(child, f"{route}.{key}" if route else str(key))
        elif isinstance(node, list):
            for index, child in enumerate(node):
                visit(child, f"{route}[{index}]")

    visit(summary, "")
    if untyped:
        raise ValidationError(
            f"{label} contains path/hash bindings outside its typed retained inventory: "
            + ", ".join(untyped[:8])
        )


def _rehash_typed_artifacts(
    name: str,
    summary: dict[str, Any],
    owner_path: Path,
    tracker: FileTracker,
) -> list[dict[str, Any]]:
    typed: list[dict[str, Any]] = []
    if name == "offline_proof":
        artifacts = _typed_manifest(
            raw=summary.get("artifact_manifest"),
            owner_path=owner_path,
            label="offline proof artifact_manifest",
            count=146,
            tracker=tracker,
            exact_fields_for_role=lambda role: (
                {
                    "role",
                    "path",
                    "sha256",
                    "cell",
                    "source_role",
                    "run_id",
                    "process_instance_id",
                    "arm",
                    "waypoints",
                    "workers",
                }
                if role == "proof_cell_artifact"
                else {"role", "path", "sha256"}
            ),
        )
        _roles_exact(
            artifacts,
            {
                "proof_cell_artifact": 144,
                "baseline_clean_checkout_manifest": 1,
                "baseline_qualification_patch": 1,
            },
            "offline proof artifact_manifest",
        )
        tools = _typed_manifest(
            raw=summary.get("tool_manifest"),
            owner_path=owner_path,
            label="offline proof tool_manifest",
            count=9,
            tracker=tracker,
            exact_fields_for_role=lambda ignored: {"role", "path", "sha256"},
        )
        _roles_exact(
            tools,
            {role: 1 for role in (
                "proof_verifier",
                "proof_wrapper",
                "source_identity_helper",
                "current_proof_runner",
                "baseline_proof_runner",
                "baseline_prepare_runner",
                "baseline_policy_compat_patch",
                "baseline_free_space_profile_patch",
                "baseline_free_space_query_profile_patch",
            )},
            "offline proof tool_manifest",
        )
        typed.extend((*artifacts, *tools))
    elif name == "scheduled_runtime":
        artifacts = _typed_manifest(
            raw=summary.get("artifact_manifest"),
            owner_path=owner_path,
            label="scheduled runtime artifact_manifest",
            count=28,
            tracker=tracker,
            exact_fields_for_role=lambda role: (
                {
                    "role",
                    "path",
                    "sha256",
                    "arm",
                    "measurement_kind",
                    "run_id",
                    "process_instance_id",
                }
                if role == "runtime_schedule_artifact"
                else {"role", "path", "sha256"}
            ),
        )
        _roles_exact(
            artifacts,
            {
                "runtime_schedule_artifact": 24,
                "baseline_clean_checkout_manifest": 1,
                "baseline_harness_patch": 1,
                "baseline_observation_patch": 1,
                "baseline_decision_parity_artifact": 1,
            },
            "scheduled runtime artifact_manifest",
        )
        probes = _typed_manifest(
            raw=summary.get("parity_probe_manifest"),
            owner_path=owner_path,
            label="scheduled runtime parity_probe_manifest",
            count=2,
            tracker=tracker,
            exact_fields_for_role=lambda ignored: {"role", "path", "sha256"},
        )
        _roles_exact(
            probes,
            {
                "baseline_timing_parity_probe": 1,
                "baseline_observation_parity_probe": 1,
            },
            "scheduled runtime parity_probe_manifest",
        )
        tools = _typed_manifest(
            raw=summary.get("tool_manifest"),
            owner_path=owner_path,
            label="scheduled runtime tool_manifest",
            count=11,
            tracker=tracker,
            exact_fields_for_role=lambda ignored: {"role", "path", "sha256"},
        )
        _roles_exact(
            tools,
            {role: 1 for role in (
                "runtime_schedule_verifier",
                "runtime_artifact_wrapper",
                "source_identity_helper",
                "current_runtime_runner",
                "baseline_runtime_runner",
                "baseline_runtime_prepare",
                "baseline_parity_generator",
                "baseline_observation_patch",
                "runtime_harness_source",
                "baseline_runtime_cmake_patch",
                "baseline_runtime_compat_patch",
            )},
            "scheduled runtime tool_manifest",
        )
        typed.extend((*artifacts, *probes, *tools))
    elif name == "live_contact_modes":
        live = _typed_manifest(
            raw=summary.get("artifact_manifest"),
            owner_path=owner_path,
            label="live artifact_manifest",
            count=96,
            tracker=tracker,
            exact_fields_for_role=lambda role: (
                {"role", "row_id", "path", "sha256"}
                if role
                in {
                    "live_row_evidence",
                    "live_row_controller_log",
                    "live_row_metadata",
                }
                else {"role", "path", "sha256"}
            ),
        )
        _roles_exact(
            live,
            {
                "live_manifest": 1,
                "live_verifier": 1,
                "baseline_observation_patch": 1,
                "baseline_observation_parity": 1,
                "live_row_runner": 1,
                "live_row_evidence": 16,
                "live_row_controller_log": 16,
                "live_row_metadata": 16,
                "live_mechanism_replay_input": 43,
            },
            "live artifact_manifest",
        )
        rows = summary.get("rows")
        row_ids = [row.get("id") for row in rows] if isinstance(rows, list) else []
        raw_live = summary.get("artifact_manifest")
        for role in (
            "live_row_evidence",
            "live_row_controller_log",
            "live_row_metadata",
        ):
            observed = [
                item.get("row_id")
                for item in raw_live
                if isinstance(item, dict) and item.get("role") == role
            ]
            if observed != row_ids:
                raise ValidationError(f"live {role} inventory does not match row order")
        typed.extend(live)
    elif name == "loaded_kernel":
        loaded = _typed_manifest(
            raw=summary.get("retained_artifact_manifest"),
            owner_path=owner_path,
            label="loaded-kernel retained_artifact_manifest",
            count=39,
            tracker=tracker,
            exact_fields_for_role=lambda role: (
                {"role", "iteration", "path", "sha256"}
                if role == "loaded_iteration_log"
                else {"role", "path", "sha256"}
            ),
        )
        _roles_exact(
            loaded,
            {
                "loaded_run_manifest": 1,
                "loaded_raw_manifest": 1,
                "loaded_container_inspect": 1,
                "loaded_container_log": 1,
                "loaded_test_binary": 1,
                "loaded_gate_script": 1,
                "loaded_verifier_script": 1,
                "loaded_ledger": 1,
                "loaded_test_inventory": 1,
                "loaded_iteration_log": 30,
            },
            "loaded-kernel retained_artifact_manifest",
        )
        raw_loaded = summary.get("retained_artifact_manifest")
        iterations = [
            item.get("iteration")
            for item in raw_loaded
            if isinstance(item, dict) and item.get("role") == "loaded_iteration_log"
        ]
        if iterations != list(range(1, 31)):
            raise ValidationError("loaded-kernel retained log order is incomplete")
        typed.extend(loaded)
    elif name == "release_gates":
        release = _typed_manifest(
            raw=summary.get("retained_artifact_manifest"),
            owner_path=owner_path,
            label="release retained_artifact_manifest",
            count=28,
            tracker=tracker,
            exact_fields_for_role=lambda role: (
                {"role", "lane", "path", "sha256", "bytes"}
                if role in {"release_container_log", "release_security_override"}
                else (
                    {"role", "lane", "path", "sha256"}
                    if role.startswith("release_")
                    and role not in {
                        "release_artifact_manifest",
                        "release_verifier",
                        "release_runner",
                    }
                    else {"role", "path", "sha256"}
                )
            ),
        )
        _roles_exact(
            release,
            {
                "release_artifact_manifest": 1,
                "release_verifier": 1,
                "release_runner": 1,
                "release_lane_manifest": 7,
                "release_container_inspect": 7,
                "release_container_log": 7,
                "release_security_override": 4,
            },
            "release retained_artifact_manifest",
        )
        raw_release = summary.get("retained_artifact_manifest")
        lanes = [lane.get("name") for lane in summary.get("lanes", [])]
        for role, expected_lanes in (
            ("release_lane_manifest", lanes),
            ("release_container_inspect", lanes),
            ("release_container_log", lanes),
            ("release_security_override", sorted(RELEASE_LANES - {
                "full_dev_regression",
                "full_ros_regression",
                "contract_architecture_hygiene",
            }, key=lanes.index)),
        ):
            observed = [
                item.get("lane")
                for item in raw_release
                if isinstance(item, dict) and item.get("role") == role
            ]
            if observed != expected_lanes:
                raise ValidationError(f"release {role} lane inventory is not exact")
        typed.extend(release)
    else:  # pragma: no cover - exact caller inventory owns this enum
        raise ValidationError(f"unknown Step 18 evidence family: {name}")

    _require_all_summary_bindings_typed(summary, owner_path, name, typed)
    retained: dict[str, str] = {}
    for item in typed:
        prior = retained.get(item["path"])
        if prior is not None and prior != item["sha256"]:
            raise ValidationError(
                f"{name} assigns conflicting hashes to {item['path']}"
            )
        retained[item["path"]] = item["sha256"]
    return [
        {"path": path, "sha256": digest}
        for path, digest in sorted(retained.items())
    ]


def _require_common_candidate(
    summary: dict[str, Any],
    label: str,
    source_field: str,
    specs_source_field: str,
    contract_field: str,
    source: dict[str, Any],
    specs_source: dict[str, Any],
    contract: dict[str, Any],
) -> None:
    observed_source = _identity(summary.get(source_field), f"{label}.{source_field}")
    observed_specs_source = _identity(
        summary.get(specs_source_field), f"{label}.{specs_source_field}"
    )
    observed_contract = _contract_identity(
        summary.get(contract_field), f"{label}.{contract_field}"
    )
    if observed_source != source:
        raise ValidationError(f"{label} was not run on the common ManyForge candidate")
    if observed_specs_source != specs_source:
        raise ValidationError(f"{label} was not run on the common Specs source")
    if observed_contract != contract:
        raise ValidationError(f"{label} was not run on the common Specs contract")


def _passing_checks(summary: dict[str, Any], label: str) -> None:
    checks = summary.get("checks")
    if not isinstance(checks, list) or not checks:
        raise ValidationError(f"{label} has no structured verifier checks")
    names: list[str] = []
    for check in checks:
        if (
            not isinstance(check, dict)
            or not isinstance(check.get("name"), str)
            or not check["name"]
            or check.get("passed") is not True
        ):
            raise ValidationError(f"{label} contains a failed or malformed check")
        names.append(check["name"])
    if len(names) != len(set(names)):
        raise ValidationError(f"{label} contains duplicate check names")


def _unique_manifest_paths(
    artifact_manifest: Any, expected_count: int, label: str
) -> None:
    if not isinstance(artifact_manifest, list) or len(artifact_manifest) != expected_count:
        raise ValidationError(f"{label} artifact-manifest count is not {expected_count}")
    paths: list[str] = []
    for artifact in artifact_manifest:
        if (
            not isinstance(artifact, dict)
            or not isinstance(artifact.get("path"), str)
            or not artifact["path"]
            or SHA256.fullmatch(str(artifact.get("sha256", ""))) is None
        ):
            raise ValidationError(f"{label} artifact manifest is malformed")
        paths.append(str(Path(artifact["path"]).resolve()))
    if len(paths) != len(set(paths)):
        raise ValidationError(f"{label} artifact paths are not globally unique")


def _verify_precise_reference_parity(summary: dict[str, Any]) -> None:
    parity = summary.get("precise_reference_parity")
    parity_fields = {
        "timing_gate_excluded",
        "record_count",
        "runs_per_source_role",
        "case_count",
        "case_order",
        "small_case_worker_counts",
        "normalization_contract_id",
        "normalization_contract_sha256",
        "orchestration_projection_contract_id",
        "orchestration_projection_contract_sha256",
        "input_corpus_sha256",
        "baseline_transcript_sha256",
        "current_transcript_sha256",
        "semantic_parity",
        "matched_profile_full_stable_parity",
        "cross_worker_orchestration_projection",
        "per_record_transcripts",
        "normalization_audit",
        "partition_inventory",
        "completion_order_inventory",
        "exceptional_cases",
    }
    if not isinstance(parity, dict):
        raise ValidationError(
            "offline proof precise-reference parity result is missing"
        )
    _exact_keys(parity, parity_fields, "precise-reference parity")
    performance = summary.get("performance")

    def contains_parity_timing_key(value: Any) -> bool:
        if isinstance(value, dict):
            return "precise_reference_parity" in value or any(
                contains_parity_timing_key(child) for child in value.values()
            )
        if isinstance(value, list):
            return any(contains_parity_timing_key(child) for child in value)
        return False

    if contains_parity_timing_key(performance):
        raise ValidationError("precise-reference parity was included in timing gates")
    if (
        parity.get("timing_gate_excluded") is not True
        or parity.get("record_count") != 6
        or parity.get("runs_per_source_role") != 3
        or parity.get("case_count") != len(PRECISE_REFERENCE_CASE_ORDER)
        or parity.get("case_order") != list(PRECISE_REFERENCE_CASE_ORDER)
        or parity.get("small_case_worker_counts") != [1, 2, 3, 4, 5]
        or parity.get("normalization_contract_id")
        != "step18_precise_reference_normalization.v1"
        or parity.get("orchestration_projection_contract_id")
        != "step18_cross_worker_orchestration_projection.v1"
        or parity.get("semantic_parity") is not True
        or parity.get("matched_profile_full_stable_parity") is not True
    ):
        raise ValidationError(
            "offline proof precise-reference parity profile is incomplete"
        )
    for field in (
        "normalization_contract_sha256",
        "orchestration_projection_contract_sha256",
        "input_corpus_sha256",
        "baseline_transcript_sha256",
        "current_transcript_sha256",
    ):
        _sha256_value(parity.get(field), f"precise-reference parity {field}")
    if parity["baseline_transcript_sha256"] != parity["current_transcript_sha256"]:
        raise ValidationError("precise-reference normalized transcripts differ")

    worker_keys = {str(worker) for worker in range(1, 6)}
    completion_keys = {
        "serial",
        *("-".join(map(str, order)) for order in PRECISE_REFERENCE_COMPLETION_ORDERS),
    }
    exceptional_digest_keys = {
        "pre_cancelled",
        "expired_absolute_deadline",
        "worker_exception",
    }

    def hash_map(value: Any, keys: set[str], label: str) -> dict[str, str]:
        if not isinstance(value, dict):
            raise ValidationError(f"{label} must be an object")
        _exact_keys(value, keys, label)
        return {
            key: _sha256_value(item, f"{label}.{key}")
            for key, item in value.items()
        }

    projection = parity.get("cross_worker_orchestration_projection")
    if not isinstance(projection, dict):
        raise ValidationError("precise-reference orchestration projection is missing")
    _exact_keys(
        projection,
        {
            "excluded_result_fields",
            "small_case_profile_sha256",
            "partition_profile_sha256",
            "completion_sha256",
            "parity",
        },
        "precise-reference orchestration projection",
    )
    if (
        projection.get("excluded_result_fields") != ["query_id"]
        or projection.get("parity") is not True
    ):
        raise ValidationError("precise-reference orchestration projection is not exact")
    projected_small = hash_map(
        projection.get("small_case_profile_sha256"),
        worker_keys,
        "precise-reference projected small profiles",
    )
    projected_partition = hash_map(
        projection.get("partition_profile_sha256"),
        worker_keys,
        "precise-reference projected partition profiles",
    )
    projected_completion = hash_map(
        projection.get("completion_sha256"),
        completion_keys,
        "precise-reference projected completion profiles",
    )
    if any(
        len(set(values.values())) != 1
        for values in (projected_small, projected_partition, projected_completion)
    ):
        raise ValidationError(
            "precise-reference cross-worker orchestration projection differs"
        )

    manifest = summary.get("artifact_manifest")
    parity_records = [
        artifact
        for artifact in manifest
        if isinstance(artifact, dict)
        and artifact.get("role") == "proof_cell_artifact"
        and artifact.get("cell") == "precise_reference_parity"
    ] if isinstance(manifest, list) else []
    manifest_fields = {
        "role",
        "path",
        "sha256",
        "cell",
        "source_role",
        "run_id",
        "process_instance_id",
        "arm",
        "waypoints",
        "workers",
    }
    for index, record in enumerate(parity_records):
        _exact_keys(
            record,
            manifest_fields,
            f"precise-reference raw artifact[{index}]",
        )
        if any(
            record.get(field) is not None
            for field in ("arm", "waypoints", "workers")
        ):
            raise ValidationError(
                "precise-reference raw artifact contains a fabricated benchmark profile"
            )
    source_roles = [record.get("source_role") for record in parity_records]
    if (
        len(parity_records) != 6
        or source_roles.count("pristine_baseline_timing") != 3
        or source_roles.count("current_candidate") != 3
        or len({record.get("run_id") for record in parity_records}) != 6
        or len({record.get("process_instance_id") for record in parity_records}) != 6
        or len({record.get("sha256") for record in parity_records}) != 6
    ):
        raise ValidationError("precise-reference raw artifact inventory is not exact")
    manifest_by_identity = {
        (
            record.get("source_role"),
            record.get("run_id"),
            record.get("process_instance_id"),
        ): record
        for record in parity_records
    }
    if len(manifest_by_identity) != 6:
        raise ValidationError(
            "precise-reference raw artifact identities are not unique"
        )

    per_record = parity.get("per_record_transcripts")
    if not isinstance(per_record, list) or len(per_record) != 6:
        raise ValidationError(
            "precise-reference per-record transcript inventory is incomplete"
        )
    per_record_fields = {
        "source_role",
        "repetition",
        "run_id",
        "process_instance_id",
        "artifact_sha256",
        "input_corpus_sha256",
        "normalized_transcript_sha256",
        "small_case_profile_sha256",
        "small_case_sha256",
        "partition_profile_sha256",
        "completion_order_sha256",
        "exceptional_case_sha256",
    }
    expected_repetitions = [
        (role, repetition)
        for role in ("pristine_baseline_timing", "current_candidate")
        for repetition in range(1, 4)
    ]
    if [
        (record.get("source_role"), record.get("repetition"))
        for record in per_record
        if isinstance(record, dict)
    ] != expected_repetitions:
        raise ValidationError("precise-reference repetition inventory is not exact")
    stable_digest_inventories: list[dict[str, Any]] = []
    for index, record in enumerate(per_record):
        if not isinstance(record, dict):
            raise ValidationError(
                f"precise-reference transcript[{index}] is not an object"
            )
        _exact_keys(
            record,
            per_record_fields,
            f"precise-reference transcript[{index}]",
        )
        role = record["source_role"]
        run_id = record["run_id"]
        process_id = record["process_instance_id"]
        if not all(
            isinstance(value, str) and value
            for value in (role, run_id, process_id)
        ):
            raise ValidationError(
                f"precise-reference transcript[{index}] identity is malformed"
            )
        raw_record = manifest_by_identity.get((role, run_id, process_id))
        artifact_sha = _sha256_value(
            record.get("artifact_sha256"),
            f"precise-reference transcript[{index}] artifact",
        )
        if raw_record is None or artifact_sha != raw_record.get("sha256"):
            raise ValidationError(
                f"precise-reference transcript[{index}] is not bound to its "
                "raw artifact"
            )
        if (
            _sha256_value(
                record.get("input_corpus_sha256"),
                f"precise-reference transcript[{index}] input corpus",
            )
            != parity["input_corpus_sha256"]
        ):
            raise ValidationError(
                f"precise-reference transcript[{index}] input corpus differs"
            )
        expected_transcript = (
            parity["baseline_transcript_sha256"]
            if role == "pristine_baseline_timing"
            else parity["current_transcript_sha256"]
        )
        if (
            _sha256_value(
                record.get("normalized_transcript_sha256"),
                f"precise-reference transcript[{index}] normalized transcript",
            )
            != expected_transcript
        ):
            raise ValidationError(
                f"precise-reference transcript[{index}] digest differs"
            )
        small_profiles = hash_map(
            record.get("small_case_profile_sha256"),
            worker_keys,
            f"precise-reference transcript[{index}] small profiles",
        )
        small_cases = record.get("small_case_sha256")
        if not isinstance(small_cases, dict):
            raise ValidationError(
                f"precise-reference transcript[{index}] per-case digests are missing"
            )
        _exact_keys(
            small_cases,
            worker_keys,
            f"precise-reference transcript[{index}] per-case digests",
        )
        normalized_small_cases = {
            worker: hash_map(
                values,
                set(PRECISE_REFERENCE_CASE_ORDER),
                f"precise-reference transcript[{index}] per-case digests.{worker}",
            )
            for worker, values in small_cases.items()
        }
        partition_profiles = hash_map(
            record.get("partition_profile_sha256"),
            worker_keys,
            f"precise-reference transcript[{index}] partition profiles",
        )
        completion_profiles = hash_map(
            record.get("completion_order_sha256"),
            completion_keys,
            f"precise-reference transcript[{index}] completion profiles",
        )
        exceptional_profiles = hash_map(
            record.get("exceptional_case_sha256"),
            exceptional_digest_keys,
            f"precise-reference transcript[{index}] exceptional profiles",
        )
        stable_digest_inventories.append(
            {
                "small_case_profile_sha256": small_profiles,
                "small_case_sha256": normalized_small_cases,
                "partition_profile_sha256": partition_profiles,
                "completion_order_sha256": completion_profiles,
                "exceptional_case_sha256": exceptional_profiles,
            }
        )
    if any(
        inventory != stable_digest_inventories[0]
        for inventory in stable_digest_inventories[1:]
    ):
        raise ValidationError(
            "precise-reference per-record stable transcript digests differ"
        )
    for role in ("pristine_baseline_timing", "current_candidate"):
        role_runs = [
            record["run_id"]
            for record in per_record
            if record["source_role"] == role
        ]
        if role_runs != sorted(role_runs):
            raise ValidationError(
                f"precise-reference {role} repetition order is not deterministic"
            )

    partition = parity.get("partition_inventory")
    if not isinstance(partition, dict):
        raise ValidationError("precise-reference partition inventory is missing")
    _exact_keys(
        partition,
        {
            "sample_kind_count",
            "waypoint_count",
            "patterns_per_worker",
            "worker_counts",
            "total_partition_evaluations_per_record",
            "serial_reference_evaluations_per_record",
            "matched_profile_sha256",
            "parity",
        },
        "precise-reference partition inventory",
    )
    partition_profiles = hash_map(
        partition.get("matched_profile_sha256"),
        worker_keys,
        "precise-reference matched partition profiles",
    )
    if any(
        (
            partition.get("sample_kind_count") != 4,
            partition.get("waypoint_count") != 5,
            partition.get("patterns_per_worker") != 1024,
            partition.get("worker_counts") != [2, 3, 4, 5],
            partition.get("total_partition_evaluations_per_record") != 4096,
            partition.get("serial_reference_evaluations_per_record") != 1024,
            partition_profiles
            != stable_digest_inventories[0]["partition_profile_sha256"],
            partition.get("parity") is not True,
        )
    ):
        raise ValidationError("precise-reference partition inventory is not exhaustive")

    completion = parity.get("completion_order_inventory")
    orders = completion.get("orders") if isinstance(completion, dict) else None
    normalized_orders = (
        tuple(tuple(order) for order in orders)
        if isinstance(orders, list)
        and all(isinstance(order, list) for order in orders)
        else ()
    )
    if not isinstance(completion, dict):
        raise ValidationError("precise-reference completion-order inventory is missing")
    _exact_keys(
        completion,
        {
            "workers",
            "permutation_count",
            "orders",
            "matched_profile_sha256",
            "parity",
        },
        "precise-reference completion-order inventory",
    )
    completion_profiles = hash_map(
        completion.get("matched_profile_sha256"),
        completion_keys,
        "precise-reference matched completion profiles",
    )
    if (
        completion.get("workers") != 3
        or completion.get("permutation_count") != 6
        or normalized_orders != PRECISE_REFERENCE_COMPLETION_ORDERS
        or completion_profiles
        != stable_digest_inventories[0]["completion_order_sha256"]
        or completion.get("parity") is not True
    ):
        raise ValidationError(
            "precise-reference completion-order inventory is incomplete"
        )

    exceptional = parity.get("exceptional_cases")
    expected_exceptional = {
        "pre_cancel": {
            "reason": "operator_stop",
            "status": "not_invoked",
            "status_code": None,
            "earliest": {"waypoint": 0, "time_ns": 0},
            "parity": True,
        },
        "expired_deadline": {
            "reason": "collision_validation_wall_timeout",
            "status": "deadline_exceeded",
            "status_code": 1,
            "earliest": {"waypoint": 0, "time_ns": None},
            "parity": True,
        },
        "worker_exception": {
            "reason": "collision_validation_worker_exception",
            "status": "engine_error",
            "status_code": 5,
            "earliest": {"waypoint": 4, "time_ns": 40_000_000},
            "parity": True,
        },
    }
    if exceptional != expected_exceptional:
        raise ValidationError("precise-reference exceptional profile is incomplete")

    audit = parity.get("normalization_audit")
    if not isinstance(audit, dict):
        raise ValidationError("precise-reference normalization audit is missing")
    _exact_keys(
        audit,
        {"pristine_baseline_timing", "current_candidate"},
        "precise-reference normalization audit",
    )
    audit_fields = {
        "baseline_legacy_zero_observations",
        "baseline_implied_signed_mode_observations",
        "current_explicit_signed_mode_observations",
        "baseline_raw_mode_absent_profiles",
        "current_raw_mode_present_profiles",
        "baseline_missing_metric_code3_observations",
        "current_missing_metric_code8_observations",
        "omitted_global_volatile_diagnostic_values",
    }
    baseline_audit = audit.get("pristine_baseline_timing")
    current_audit = audit.get("current_candidate")
    for role, role_audit in audit.items():
        if not isinstance(role_audit, dict):
            raise ValidationError(f"precise-reference {role} audit is malformed")
        _exact_keys(role_audit, audit_fields, f"precise-reference {role} audit")
        if not all(
            isinstance(value, int) and not isinstance(value, bool) and value >= 0
            for value in role_audit.values()
        ):
            raise ValidationError(f"precise-reference {role} audit is malformed")
    if not (
        baseline_audit["baseline_legacy_zero_observations"] > 0
        and baseline_audit["baseline_legacy_zero_observations"]
        == baseline_audit["baseline_implied_signed_mode_observations"]
        and baseline_audit["current_explicit_signed_mode_observations"] == 0
        and baseline_audit["baseline_raw_mode_absent_profiles"] == 50
        and baseline_audit["current_raw_mode_present_profiles"] == 0
        and baseline_audit["baseline_missing_metric_code3_observations"] == 5
        and baseline_audit["current_missing_metric_code8_observations"] == 0
        and baseline_audit["omitted_global_volatile_diagnostic_values"] == 0
        and current_audit["baseline_legacy_zero_observations"] == 0
        and current_audit["baseline_implied_signed_mode_observations"] == 0
        and current_audit["current_explicit_signed_mode_observations"] > 0
        and current_audit["baseline_raw_mode_absent_profiles"] == 0
        and current_audit["current_raw_mode_present_profiles"] == 50
        and current_audit["baseline_missing_metric_code3_observations"] == 0
        and current_audit["current_missing_metric_code8_observations"] == 5
        and current_audit["omitted_global_volatile_diagnostic_values"] == 0
    ):
        raise ValidationError("precise-reference normalization audit is not exact")


def _verify_proof(
    summary: dict[str, Any],
    source: dict[str, Any],
    specs_source: dict[str, Any],
    contract: dict[str, Any],
) -> None:
    if summary.get("schema_version") != PROOF_SCHEMA:
        raise ValidationError("offline proof summary schema changed")
    if (
        summary.get("verification_scope") != "offline_proof_cells"
        or summary.get("minimum_runs") != 3
        or summary.get("development_smoke") is not False
        or summary.get("offline_proof_gate_passed") is not True
        or summary.get("verification_passed") is not True
        or summary.get("failure_count") != 0
        or summary.get("record_count") != 144
        or summary.get("overall_step18_closure_claimed") is not False
        or summary.get("requires_common_candidate_aggregator") is not True
    ):
        raise ValidationError("offline proof is not the complete passing formal profile")
    _unique_manifest_paths(summary.get("artifact_manifest"), 146, "offline proof")
    _passing_checks(summary, "offline proof")
    if not isinstance(summary.get("performance"), dict) or not summary["performance"]:
        raise ValidationError("offline proof performance result is missing")
    _verify_precise_reference_parity(summary)
    _require_common_candidate(
        summary,
        "offline proof",
        "candidate_source_identity",
        "candidate_specs_source_identity",
        "candidate_specs_contract_identity",
        source,
        specs_source,
        contract,
    )


def _verify_runtime(
    summary: dict[str, Any],
    source: dict[str, Any],
    specs_source: dict[str, Any],
    contract: dict[str, Any],
) -> None:
    if summary.get("schema_version") != RUNTIME_SCHEMA:
        raise ValidationError("scheduled runtime summary schema changed")
    if (
        summary.get("record_count") != 24
        or summary.get("minimum_runs") != 3
        or summary.get("baseline_required") is not True
        or summary.get("genuine_pristine_baseline_present") is not True
        or summary.get("before_after_measured") is not True
        or summary.get("available_checks_passed") is not True
        or summary.get("runtime_schedule_subgate_passed") is not True
        or summary.get("failure_count") != 0
        or summary.get("overall_step18_closure_claimed") is not False
        or summary.get("requires_common_candidate_aggregator") is not True
    ):
        raise ValidationError("scheduled runtime gate is incomplete or failed")
    _unique_manifest_paths(
        summary.get("artifact_manifest"), 28, "scheduled runtime"
    )
    _passing_checks(summary, "scheduled runtime")
    profile = summary.get("formal_profile")
    if not isinstance(profile, dict) or not profile:
        raise ValidationError("scheduled runtime formal profile is missing")
    threshold = summary.get("formal_bounded_threshold_m")
    if (
        not isinstance(threshold, (int, float))
        or isinstance(threshold, bool)
        or not math.isfinite(float(threshold))
        or float(threshold) < 0.0
    ):
        raise ValidationError("scheduled runtime bounded threshold is invalid")
    _require_common_candidate(
        summary,
        "scheduled runtime",
        "candidate_source_identity",
        "candidate_specs_source_identity",
        "candidate_specs_contract_identity",
        source,
        specs_source,
        contract,
    )


def _verify_live(
    summary: dict[str, Any],
    source: dict[str, Any],
    specs_source: dict[str, Any],
    contract: dict[str, Any],
) -> None:
    if summary.get("schema_version") != LIVE_SCHEMA:
        raise ValidationError("live contact-mode summary schema changed")
    rows = summary.get("rows")
    comparisons = summary.get("comparisons")
    if (
        summary.get("scope")
        != "step18_live_validation_nonregression_subverification"
        or summary.get("live_lane_passed") is not True
        or summary.get("overall_step18_closure_claimed") is not False
        or summary.get("failures") != []
        or not isinstance(rows, list)
        or len(rows) != 16
        or not isinstance(comparisons, list)
        or len(comparisons) != 6
        or any(
            not isinstance(comparison, dict)
            or comparison.get("passed") is not True
            for comparison in comparisons
        )
        or summary.get("pinned_baseline_commit") != PINNED_BASELINE_COMMIT
        or summary.get("pinned_specs_commit") != PINNED_SPECS_COMMIT
    ):
        raise ValidationError("live sixteen-row gate is incomplete or failed")
    _require_common_candidate(
        summary,
        "live contact modes",
        "candidate_source_identity",
        "candidate_specs_source_identity",
        "candidate_specs_contract_identity",
        source,
        specs_source,
        contract,
    )


def _verify_loaded(
    summary: dict[str, Any],
    source: dict[str, Any],
    specs_source: dict[str, Any],
    contract: dict[str, Any],
) -> None:
    if summary.get("schema_version") != LOADED_SCHEMA:
        raise ValidationError("loaded-kernel summary schema changed")
    configuration = summary.get("configuration")
    logs = summary.get("iteration_logs")
    test_inventory = summary.get("test_inventory")
    if (
        summary.get("loaded_kernel_gate_passed") is not True
        or summary.get("failure_count") != 0
        or summary.get("step18_closed") is not False
        or not isinstance(configuration, dict)
        or configuration.get("iterations") != 30
        or configuration.get("load_workers") != 3
        or configuration.get("timeout_seconds") != 30
        or not isinstance(configuration.get("tests_per_iteration"), int)
        or configuration["tests_per_iteration"] < 291
        or not isinstance(logs, list)
        or len(logs) != 30
        or not isinstance(test_inventory, dict)
        or set(test_inventory) != {"path", "sha256", "required_predicates"}
        or test_inventory.get("required_predicates")
        != sorted(REQUIRED_LOADED_PREDICATE_TESTS)
    ):
        raise ValidationError("loaded-kernel gate is incomplete or failed")
    _require_common_candidate(
        summary,
        "loaded kernel",
        "source_identity",
        "specs_source_identity_at_run",
        "specs_contract_identity",
        source,
        specs_source,
        contract,
    )


def _verify_release(
    summary: dict[str, Any],
    source: dict[str, Any],
    specs_source: dict[str, Any],
    contract: dict[str, Any],
) -> None:
    if summary.get("schema_version") != RELEASE_SCHEMA:
        raise ValidationError("release-gate summary schema changed")
    lanes = summary.get("lanes")
    if not isinstance(lanes, list) or any(not isinstance(lane, dict) for lane in lanes):
        raise ValidationError("release-gate lanes are not structured")
    names = [lane.get("name") for lane in lanes]
    if (
        summary.get("verification_scope") != "corrected_source_release_gates"
        or summary.get("release_gates_passed") is not True
        or summary.get("failure_count") != 0
        or summary.get("overall_step18_closure_claimed") is not False
        or summary.get("requires_common_candidate_aggregator") is not True
        or len(names) != len(RELEASE_LANES)
        or set(names) != RELEASE_LANES
        or any(lane.get("passed") is not True for lane in lanes)
    ):
        raise ValidationError("corrected-source release gates are incomplete or failed")
    _require_common_candidate(
        summary,
        "release gates",
        "candidate_source_identity",
        "candidate_specs_source_identity",
        "candidate_specs_contract_identity",
        source,
        specs_source,
        contract,
    )


def _verify_loaded_release_image_parity(
    loaded: dict[str, Any], release: dict[str, Any]
) -> dict[str, Any]:
    loaded_container = loaded.get("container")
    if not isinstance(loaded_container, dict):
        raise ValidationError("loaded-kernel container provenance is missing")
    loaded_image = _sha256_value(
        loaded_container.get("image_id"), "loaded-kernel container image id"
    )
    lanes = release.get("lanes")
    if not isinstance(lanes, list):
        raise ValidationError("release-gate lane provenance is missing")
    dev_matches = [
        lane
        for lane in lanes
        if isinstance(lane, dict) and lane.get("name") == "full_dev_regression"
    ]
    if len(dev_matches) != 1 or not isinstance(dev_matches[0].get("container"), dict):
        raise ValidationError("release full-dev container provenance is not unique")
    release_image = _sha256_value(
        dev_matches[0]["container"].get("image_id"),
        "release full-dev container image id",
    )
    if loaded_image != release_image:
        raise ValidationError(
            "loaded-kernel and full-dev release gates used different dev images"
        )
    return {
        "loaded_kernel_image_id": loaded_image,
        "full_dev_release_image_id": release_image,
        "passed": True,
    }


def _verify_live_mechanism_ownership(live: dict[str, Any]) -> dict[str, Any]:
    parity = live.get("baseline_observation_parity")
    if (
        not isinstance(parity, dict)
        or parity.get("scope") != "retained_live_decision_trajectory_outputs"
    ):
        raise ValidationError("live retained-output parity is missing or ambiguous")
    mechanism = live.get("live_mechanism_replay")
    if (
        not isinstance(mechanism, dict)
        or mechanism.get("owner_scope")
        != "exact_ur10e_isaac_live_validation_mechanism_owner"
        or mechanism.get("observation_row_count") != 8
        or mechanism.get("all_direct_mechanism_checks_passed") is not True
        or mechanism.get("all_physical_pair_parity_checks_passed") is not True
    ):
        raise ValidationError(
            "live UR10/Isaac mechanism ownership is missing or failed"
        )
    records = mechanism.get("records")
    if (
        not isinstance(records, list)
        or len(records) != 8
        or any(
            not isinstance(record, dict)
            or record.get("physical_pair_parity_passed") is not True
            or record.get("observation_nonperturbation_passed") is not True
            or not isinstance(record.get("direct_mechanism"), dict)
            or not record["direct_mechanism"]
            for record in records
        )
    ):
        raise ValidationError(
            "live UR10/Isaac mechanism records are incomplete"
        )
    row_ids = [record.get("observation_row_id") for record in records]
    if any(not isinstance(row_id, str) or not row_id for row_id in row_ids) or len(
        set(row_ids)
    ) != 8:
        raise ValidationError("live UR10/Isaac mechanism row ownership is ambiguous")
    manifest = mechanism.get("manifest")
    runner = mechanism.get("runner")
    if any(
        not isinstance(binding, dict)
        or set(binding) != {"path", "sha256"}
        or SHA256.fullmatch(str(binding.get("sha256") or "")) is None
        for binding in (manifest, runner)
    ):
        raise ValidationError("live UR10/Isaac mechanism provenance is incomplete")
    return {
        "owner_scope": mechanism["owner_scope"],
        "mechanism_manifest": manifest,
        "mechanism_runner": runner,
        "observation_row_ids": row_ids,
        "direct_counter_record_count": len(records),
        "same_fixture": "ur10e_isaac_drop_approach",
        "passed": True,
    }


def _distance_capability_contract_hygiene(repo_root: Path) -> dict[str, Any]:
    paths = {
        "types": repo_root / "manyforge_core/include/manyforge_core/types/collision.hpp",
        "interface": repo_root
        / "manyforge_core/include/manyforge_core/collision/i_collision_query_engine.hpp",
        "engine": repo_root / "manyforge_collision/src/manyforge_collision_engine.cpp",
        "coordinator": repo_root
        / "manyforge_core/src/collision_validation_coordinator.cpp",
        "runtime": repo_root / "manyforge_core/src/runtime_collision_query_session.cpp",
    }
    texts: dict[str, str] = {}
    for name, path in paths.items():
        try:
            texts[name] = path.read_text(encoding="utf-8")
        except OSError as error:
            raise ValidationError(
                f"cannot inspect exact distance-capability source {path}: {error}"
            ) from error

    types = texts["types"]
    if len(
        re.findall(
            r"std::vector\s*<\s*CollisionDistanceRequestMode\s*>\s*"
            r"supported_distance_request_modes\s*;",
            types,
        )
    ) != 1:
        raise ValidationError(
            "distance capability descriptor lacks its exact mode inventory"
        )
    validity_start = types.find("collision_distance_request_modes_valid(")
    exact_start = types.find("collision_engine_supports_distance_request_mode(")
    exact_end = types.find("/// Compare-mode policy", exact_start)
    if validity_start < 0 or exact_start < 0 or exact_end < 0:
        raise ValidationError("shared exact distance-scope admission helpers are missing")
    validity = types[validity_start:exact_start]
    exact = types[exact_start:exact_end]
    if any(
        fragment not in validity
        for fragment in (
            "modes.empty()",
            "static_cast<int>(modes[index - 1]) >=",
            "!capability.supports_distance_query",
        )
    ):
        raise ValidationError(
            "distance-scope declaration is not validated as sorted, unique, and consistent"
        )
    if (
        "collision_distance_request_modes_valid(capability)" not in exact
        or "std::binary_search(" not in exact
        or "requested_mode" not in exact
        or "kAllCollidingPairs" in exact
        or "kAllActivePairs" in exact
    ):
        raise ValidationError(
            "distance-scope admission no longer requires one exact declared mode"
        )

    declarations = re.findall(
        r"capability_\.supported_distance_request_modes\s*=\s*\{(.*?)\};",
        texts["engine"],
        flags=re.DOTALL,
    )
    modes = (
        re.findall(r"CollisionDistanceRequestMode::(k[A-Za-z]+)", declarations[0])
        if len(declarations) == 1
        else []
    )
    expected_modes = [
        "kNone",
        "kSelectedCollidingPairs",
        "kAllCollidingPairs",
        "kAllActivePairs",
    ]
    if modes != expected_modes:
        raise ValidationError(
            "ManyForge engine does not publish the exact sorted four-mode distance inventory"
        )

    interface = texts["interface"]
    status_start = interface.find("runtime_collision_query_capability_status(")
    status_end = interface.find(
        "runtime_collision_query_capability_status_name(", status_start
    )
    if (
        status_start < 0
        or status_end < 0
        or "collision_distance_request_modes_valid(capability)"
        not in interface[status_start:status_end]
    ):
        raise ValidationError(
            "runtime capability descriptor admission omits distance-scope validation"
        )

    coordinator = texts["coordinator"]
    admission_start = coordinator.find("bool engine_admits_query_capabilities(")
    admission_end = coordinator.find(
        "CollisionResult capability_admission_failure(", admission_start
    )
    admission = (
        coordinator[admission_start:admission_end]
        if admission_start >= 0 and admission_end >= 0
        else ""
    )
    if (
        "collision_engine_supports_distance_request_mode(" not in admission
        or "capability, distance_request_mode" not in admission
        or coordinator.count("engine_admits_query_capabilities(") != 3
    ):
        raise ValidationError(
            "validation does not use exact distance-scope admission on both query paths"
        )
    required_start = coordinator.find("std::vector<std::string> required_capabilities_for(")
    required_end = coordinator.find("bool engine_admits_query_capabilities(", required_start)
    required = (
        coordinator[required_start:required_end]
        if required_start >= 0 and required_end >= 0
        else ""
    )
    if (
        "CollisionDistanceRequestMode distance_request_mode" not in required
        or "distance_request_mode != CollisionDistanceRequestMode::kNone" not in required
        or 'capabilities.push_back("penetration_depth")' not in required
    ):
        raise ValidationError(
            "validation capability requirements are not scoped to the requested mode"
        )

    runtime = texts["runtime"]
    bind_start = runtime.find("bool RuntimeCollisionQuerySession::bind_distance_selection(")
    bind_end = runtime.find("bool RuntimeCollisionQuerySession::bind_policy(", bind_start)
    binding = (
        runtime[bind_start:bind_end]
        if bind_start >= 0 and bind_end >= 0
        else ""
    )
    if (
        runtime.count("collision_engine_supports_distance_request_mode(") != 1
        or "collision_engine_supports_distance_request_mode(*capability_," not in binding
        or "requested_mode" not in binding
    ):
        raise ValidationError(
            "runtime policy binding does not use exact distance-scope admission"
        )
    if any(
        token in coordinator or token in runtime
        for token in (
            "CollisionDistanceRequestMode::kAllCollidingPairs",
            "CollisionDistanceRequestMode::kAllActivePairs",
        )
    ):
        raise ValidationError("validation/runtime contains a broader distance fallback")
    return {
        "descriptor_field_present": True,
        "declared_modes": modes,
        "declaration_sorted_unique": True,
        "exact_scope_lookup": True,
        "validation_exact_admission_paths": 2,
        "runtime_exact_admission_paths": 1,
        "mode_scoped_capability_requirements": True,
        "broader_fallback_present": False,
        "passed": True,
    }


def _production_hygiene(repo_root: Path) -> dict[str, Any]:
    obsolete_field = "min_separation_" + "clearance_m"
    text_suffixes = {
        ".cpp",
        ".cc",
        ".cxx",
        ".hpp",
        ".h",
        ".py",
        ".ts",
        ".tsx",
        ".json",
        ".yaml",
        ".yml",
    }
    production_roots = (
        "manyforge_core/include",
        "manyforge_core/src",
        "manyforge_collision/include",
        "manyforge_collision/src",
        "manyforge_roboplan/include",
        "manyforge_roboplan/src",
        "manyforge_roboplan/python",
        "manyforge_behavior/manyforge_behavior",
        "manyforge_composer/backend",
        "manyforge_composer/frontend/src",
        "examples",
        "benchmarks/roboplan_05/schema",
        "benchmarks/roboplan_05/corpus/captures",
    )
    obsolete_hits: list[str] = []
    for relative_root in production_roots:
        root = repo_root / relative_root
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in text_suffixes:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            if obsolete_field in text:
                obsolete_hits.append(str(path.relative_to(repo_root)))
    if obsolete_hits:
        raise ValidationError(
            "obsolete separation-clearance field remains in production: "
            + ", ".join(sorted(obsolete_hits))
        )

    all_active_tokens = ("kAllActivePairs", '"all_active_pairs"')
    request_owner_roots = (
        "manyforge_core/src",
        "manyforge_roboplan/src",
        "manyforge_behavior/manyforge_behavior",
        "manyforge_composer/backend",
        "manyforge_composer/frontend/src",
        "examples",
    )
    global_request_hits: list[str] = []
    for relative_root in request_owner_roots:
        root = repo_root / relative_root
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in text_suffixes:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            if any(token in text for token in all_active_tokens):
                global_request_hits.append(str(path.relative_to(repo_root)))
    if global_request_hits:
        raise ValidationError(
            "production policy/runtime owner still generates all-active distance: "
            + ", ".join(sorted(global_request_hits))
        )

    engine_path = repo_root / "manyforge_collision/src/manyforge_collision_engine.cpp"
    try:
        engine = engine_path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValidationError(f"cannot inspect collision engine: {error}") from error
    bulk_offsets = [match.start() for match in re.finditer(r"\bcomputeDistances\s*\(", engine)]
    if len(bulk_offsets) != 2 or any(
        "kAllActivePairs" not in engine[max(0, offset - 1000) : offset]
        for offset in bulk_offsets
    ):
        raise ValidationError(
            "bulk distance computation is not confined to the two explicit diagnostic arms"
        )
    selected_calls = len(re.findall(r"\bcomputeDistance\s*\(", engine))
    if selected_calls < 2:
        raise ValidationError("selected colliding-pair distance implementation is missing")
    distance_capability_contract = _distance_capability_contract_hygiene(repo_root)
    return {
        "obsolete_field_production_hits": [],
        "production_all_active_request_hits": [],
        "explicit_diagnostic_bulk_distance_sites": len(bulk_offsets),
        "selected_single_pair_distance_sites": selected_calls,
        "distance_capability_contract": distance_capability_contract,
        "passed": True,
    }


def _canonical_producer_result(name: str, value: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(value)
    if name == "live_contact_modes":
        result.pop("generated_at_utc", None)
    return result


def _run_producer(
    name: str,
    invocation: dict[str, Any],
    expected_summary: dict[str, Any],
    manyforge_root: Path,
) -> dict[str, Any]:
    verifier = Path(invocation["verifier"]["path"])
    arguments = invocation["arguments"]
    transport = invocation["result_transport"]
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    replay_root = manyforge_root / ".manyforge-var/qualification"
    try:
        replay_root.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise ValidationError(
            f"cannot create ignored producer replay root {replay_root}: {error}"
        ) from error
    try:
        ensure_ignored_output(replay_root / "replay-output.json", manyforge_root)
    except ManifestError as error:
        raise ValidationError(
            f"producer replay root is not ignored qualification evidence: {error}"
        ) from error
    with tempfile.TemporaryDirectory(
        prefix=f"step18-closure-{name}-", dir=replay_root
    ) as raw_dir:
        temporary = Path(raw_dir)
        command = [sys.executable, str(verifier), *arguments]
        output_path = temporary / "producer-summary.json"
        if transport == "output_json":
            command.extend(("--output", str(output_path)))
        try:
            completed = subprocess.run(
                command,
                cwd=manyforge_root,
                env=environment,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=300,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ValidationError(f"{name} producer replay failed to execute: {error}") from error
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise ValidationError(
                f"{name} producer replay failed with status {completed.returncode}: "
                f"{detail[-2000:]}"
            )
        if transport == "stdout_json":
            payload = completed.stdout.encode("utf-8")
        elif transport == "output_json":
            try:
                payload = output_path.read_bytes()
            except OSError as error:
                raise ValidationError(
                    f"{name} producer replay omitted its output: {error}"
                ) from error
        else:  # pragma: no cover - invocation generation owns this enum
            raise ValidationError(f"{name} producer result transport is unknown")
    observed = _json_payload(payload, f"{name} replay output")
    if _canonical_producer_result(name, observed) != _canonical_producer_result(
        name, expected_summary
    ):
        raise ValidationError(
            f"{name} summary differs from a replay of its current bound producer"
        )
    return {
        "verifier": invocation["verifier"],
        "arguments": arguments,
        "result_transport": transport,
        "result_sha256": hashlib.sha256(payload).hexdigest(),
        "canonical_match": True,
    }


def _replay_all_producers(
    *,
    declared_invocations: Any,
    summaries: dict[str, dict[str, Any]],
    summary_paths: dict[str, Path],
    manyforge_root: Path,
    tracker: FileTracker,
) -> dict[str, dict[str, Any]]:
    if not isinstance(declared_invocations, dict) or set(declared_invocations) != set(INPUTS):
        raise ValidationError("closure producer invocation inventory is not exact")
    try:
        expected_invocations = build_producer_invocations(
            summaries=summaries,
            summary_paths=summary_paths,
            manyforge_root=manyforge_root,
            tracker=tracker,
        )
    except ManifestError as error:
        raise ValidationError(f"producer invocation reconstruction failed: {error}") from error
    if declared_invocations != expected_invocations:
        raise ValidationError("closure producer invocations differ from typed summary inputs")
    if summaries["live_contact_modes"].get("producer_invocation") != expected_invocations[
        "live_contact_modes"
    ]:
        raise ValidationError(
            "live producer invocation differs from its typed retained manifest"
        )
    return {
        name: _run_producer(
            name, expected_invocations[name], summaries[name], manyforge_root
        )
        for name in INPUTS
    }


def verify(manifest_path: Path) -> dict[str, Any]:
    manifest_path = manifest_path.resolve()
    tracker = FileTracker()
    try:
        manifest_payload, manifest_digest = tracker.read(
            manifest_path, "Step 18 closure manifest"
        )
    except ManifestError as error:
        raise ValidationError(str(error)) from error
    manifest = _json_payload(manifest_payload, "Step 18 closure manifest")
    _exact_keys(
        manifest,
        {
            "schema_version",
            "manyforge_root",
            "specs_root",
            "candidate_source_identity",
            "campaign_specs_source_identity",
            "closure_specs_source_identity",
            "candidate_specs_contract_identity",
            "evidence_record",
            "inputs",
            "producer_invocations",
            "tooling",
        },
        "Step 18 closure manifest",
    )
    if manifest.get("schema_version") != MANIFEST_SCHEMA:
        raise ValidationError(f"closure manifest schema must be {MANIFEST_SCHEMA}")
    if not isinstance(manifest.get("manyforge_root"), str) or not isinstance(
        manifest.get("specs_root"), str
    ):
        raise ValidationError("closure source roots must be strings")
    manyforge_root = Path(manifest["manyforge_root"]).resolve()
    specs_root = Path(manifest["specs_root"]).resolve()
    candidate = _identity(
        manifest.get("candidate_source_identity"), "candidate_source_identity"
    )
    campaign_specs = _identity(
        manifest.get("campaign_specs_source_identity"),
        "campaign_specs_source_identity",
    )
    closure_specs = _identity(
        manifest.get("closure_specs_source_identity"),
        "closure_specs_source_identity",
    )
    candidate_contract = _contract_identity(
        manifest.get("candidate_specs_contract_identity"),
        "candidate_specs_contract_identity",
    )
    if (
        candidate["head_commit"] != PINNED_BASELINE_COMMIT
        or candidate["head_tree"] != PINNED_BASELINE_TREE
        or candidate["dirty"] is not True
        or campaign_specs["head_commit"] != PINNED_SPECS_COMMIT
        or campaign_specs["head_tree"] != PINNED_SPECS_TREE
        or campaign_specs["dirty"] is not True
        or closure_specs["head_commit"] != PINNED_SPECS_COMMIT
        or closure_specs["head_tree"] != PINNED_SPECS_TREE
        or closure_specs["dirty"] is not True
        or candidate_contract["head_commit"] != PINNED_SPECS_COMMIT
        or candidate_contract["head_tree"] != PINNED_SPECS_TREE
        or candidate_contract["contract_dirty"] is not True
    ):
        raise ValidationError("closure candidate is not the frozen Step 18 delta")
    try:
        current_candidate = source_identity(manyforge_root)
        current_specs = source_identity(specs_root)
        current_contract = specs_contract_identity(specs_root)
    except SourceIdentityError as error:
        raise ValidationError(f"cannot collect current closure identity: {error}") from error
    if current_candidate != candidate:
        raise ValidationError("current ManyForge source differs from the closure candidate")
    if current_specs != closure_specs:
        raise ValidationError("current Specs source differs from the final closure source")
    if current_contract != candidate_contract:
        raise ValidationError("current Specs contract differs from the closure candidate")
    if (
        campaign_specs["head_commit"] != candidate_contract["head_commit"]
        or campaign_specs["head_tree"] != candidate_contract["head_tree"]
    ):
        raise ValidationError("campaign Specs source and contract use different Git bases")

    evidence = manifest.get("evidence_record")
    if not isinstance(evidence, dict) or set(evidence) != {
        "path",
        "sha256",
        "schema_version",
    }:
        raise ValidationError("closure evidence record binding is not exact")
    if evidence.get("schema_version") != EVIDENCE_SCHEMA:
        raise ValidationError("closure evidence record schema changed")
    evidence_path, evidence_digest, evidence_payload = _resolve_hashed_file(
        manifest_path,
        {"path": evidence.get("path"), "sha256": evidence.get("sha256")},
        "Step 18 evidence record",
        tracker,
    )
    canonical_evidence_path = (specs_root / SPECS_CONTRACT_EXCLUDED_PATH).resolve()
    if evidence_path != canonical_evidence_path:
        raise ValidationError("closure evidence record is not the singleton excluded path")
    try:
        evidence_claim = parse_evidence_record(evidence_payload)
    except ManifestError as error:
        raise ValidationError(str(error)) from error

    tooling = manifest.get("tooling")
    if not isinstance(tooling, dict):
        raise ValidationError("closure tooling bindings must be an object")
    _exact_keys(
        tooling,
        {"generator", "verifier", "source_identity_helper"},
        "closure tooling",
    )
    expected_tools = {
        "generator": Path(__file__).with_name("generate_step18_closure_manifest.py").resolve(),
        "verifier": Path(__file__).resolve(),
        "source_identity_helper": Path(__file__).with_name("source_identity.py").resolve(),
    }
    tool_artifacts = []
    for name, expected_path in expected_tools.items():
        path, digest, _ = _resolve_hashed_file(
            manifest_path, tooling[name], f"tooling.{name}", tracker
        )
        if path != expected_path:
            raise ValidationError(f"closure tooling.{name} path is not canonical")
        tool_artifacts.append({"path": str(path), "sha256": digest})
    if evidence_claim.get("closure_verifier") != {
        "path": "scripts/qualification/verify_step18_closure.py",
        "sha256": tooling["verifier"]["sha256"],
    }:
        raise ValidationError(
            "closure evidence record does not bind the exact closure verifier"
        )

    inputs = manifest.get("inputs")
    if not isinstance(inputs, dict):
        raise ValidationError("closure inputs must be an object")
    _exact_keys(inputs, set(INPUTS), "closure inputs")
    summaries: dict[str, dict[str, Any]] = {}
    summary_paths: dict[str, Path] = {}
    input_bindings: dict[str, dict[str, str]] = {}
    all_artifacts: dict[str, list[dict[str, Any]]] = {}
    input_paths: set[Path] = set()
    for name in INPUTS:
        path, digest, payload = _resolve_hashed_file(
            manifest_path, inputs[name], f"inputs.{name}", tracker
        )
        if path in input_paths:
            raise ValidationError("closure input summary paths are not unique")
        input_paths.add(path)
        summary = _json_payload(payload, f"{name} summary")
        summaries[name] = summary
        summary_paths[name] = path
        input_bindings[name] = {"path": str(path), "sha256": digest}
        all_artifacts[name] = _rehash_typed_artifacts(
            name, summary, path, tracker
        )
        if evidence_claim["subgates"][name]["summary_sha256"] != digest:
            raise ValidationError(
                f"Step 18 evidence record does not bind the exact {name} summary"
            )

    producer_replays = _replay_all_producers(
        declared_invocations=manifest.get("producer_invocations"),
        summaries=summaries,
        summary_paths=summary_paths,
        manyforge_root=manyforge_root,
        tracker=tracker,
    )

    validators: dict[
        str,
        Callable[
            [dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]],
            None,
        ],
    ] = {
        "offline_proof": _verify_proof,
        "scheduled_runtime": _verify_runtime,
        "live_contact_modes": _verify_live,
        "loaded_kernel": _verify_loaded,
        "release_gates": _verify_release,
    }
    for name, validator in validators.items():
        validator(summaries[name], candidate, campaign_specs, candidate_contract)

    loaded_release_image_parity = _verify_loaded_release_image_parity(
        summaries["loaded_kernel"], summaries["release_gates"]
    )
    live_mechanism_ownership = _verify_live_mechanism_ownership(
        summaries["live_contact_modes"]
    )

    hygiene = _production_hygiene(manyforge_root)
    try:
        tracker.assert_unchanged()
    except ManifestError as error:
        raise ValidationError(str(error)) from error
    try:
        if source_identity(manyforge_root) != candidate:
            raise ValidationError("ManyForge source changed during closure verification")
        if source_identity(specs_root) != closure_specs:
            raise ValidationError("Specs source changed during closure verification")
        if specs_contract_identity(specs_root) != candidate_contract:
            raise ValidationError("Specs contract changed during closure verification")
    except SourceIdentityError as error:
        raise ValidationError(f"final closure identity failed: {error}") from error
    return {
        "schema_version": SUMMARY_SCHEMA,
        "verification_scope": "complete_step18_common_candidate_closure",
        "manyforge_root": str(manyforge_root),
        "specs_root": str(specs_root),
        "manifest": {"path": str(manifest_path), "sha256": manifest_digest},
        "candidate_source_identity": candidate,
        "campaign_specs_source_identity": campaign_specs,
        "closure_specs_source_identity": closure_specs,
        "candidate_specs_contract_identity": candidate_contract,
        "evidence_record": {
            "path": str(evidence_path),
            "sha256": evidence_digest,
            "schema_version": EVIDENCE_SCHEMA,
            "status": evidence_claim["status"],
        },
        "inputs": input_bindings,
        "producer_replays": producer_replays,
        "retained_artifacts": all_artifacts,
        "tooling": tool_artifacts,
        "loaded_release_image_parity": loaded_release_image_parity,
        "live_mechanism_ownership": live_mechanism_ownership,
        "production_hygiene": hygiene,
        "subgate_count": len(INPUTS),
        "failure_count": 0,
        "all_required_subgates_passed": True,
        "overall_step18_closure_claimed": True,
        "step18_closed": True,
    }


def _assert_result_bindings_current(value: Any) -> None:
    if isinstance(value, dict):
        if set(value) >= {"path", "sha256"}:
            raw_path = value.get("path")
            expected = value.get("sha256")
            if isinstance(raw_path, str) and isinstance(expected, str):
                path = Path(raw_path).resolve()
                if not path.is_file() or _sha256(path) != expected:
                    raise ValidationError(f"closure binding changed before publication: {path}")
        for child in value.values():
            _assert_result_bindings_current(child)
    elif isinstance(value, list):
        for child in value:
            _assert_result_bindings_current(child)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    temporary: Path | None = None
    try:
        result = verify(args.manifest)
        output = ensure_ignored_output(args.output, Path(result["manyforge_root"]))
        if output.exists():
            raise ValidationError(f"refusing to overwrite closure summary: {output}")
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(f".{output.name}.tmp-{os.getpid()}")
        if temporary.exists():
            raise ValidationError(f"closure temporary output already exists: {temporary}")
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        _assert_result_bindings_current(result)
        if source_identity(Path(result["manyforge_root"])) != result[
            "candidate_source_identity"
        ]:
            raise ValidationError("ManyForge source changed before closure publication")
        if source_identity(Path(result["specs_root"])) != result[
            "closure_specs_source_identity"
        ]:
            raise ValidationError("Specs source changed before closure publication")
        if specs_contract_identity(Path(result["specs_root"])) != result[
            "candidate_specs_contract_identity"
        ]:
            raise ValidationError("Specs contract changed before closure publication")
        os.link(temporary, output)
        try:
            temporary.unlink()
        except OSError:
            pass
        temporary = None
    except (OSError, ManifestError, SourceIdentityError, ValidationError, ValueError) as error:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"Step 18 closure verification failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"step18_closed": True, "subgates": len(INPUTS)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
