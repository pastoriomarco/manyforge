from __future__ import annotations

import importlib.util
import hashlib
import json
import subprocess
import sys
from collections import defaultdict
from copy import deepcopy
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
QUALIFICATION_ROOT = REPO_ROOT / "scripts" / "qualification"
WRAPPER = QUALIFICATION_ROOT / "wrap_step18_proof_artifact.py"
VERIFIER = QUALIFICATION_ROOT / "verify_step18_proof_cells.py"
sys.path.insert(0, str(QUALIFICATION_ROOT))

from source_identity import (  # noqa: E402
    SPECS_CONTRACT_EXCLUDED_PATH,
    specs_contract_fingerprint_sha256,
    source_fingerprint_sha256,
)


def _load_verifier():
    spec = importlib.util.spec_from_file_location("step18_proof_verifier", VERIFIER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _identity(character: str) -> dict[str, object]:
    head_commit = character * 40
    head_tree = chr(ord(character) + 1) * 40
    delta_sha256 = chr(ord(character) + 2) * 64
    return {
        "schema_version": "manyforge.source_identity.v1",
        "head_commit": head_commit,
        "head_tree": head_tree,
        "dirty": True,
        "delta_sha256": delta_sha256,
        "fingerprint_sha256": source_fingerprint_sha256(
            head_commit=head_commit,
            head_tree=head_tree,
            dirty=True,
            delta_sha256=delta_sha256,
        ),
    }


def _contract_identity(specs: dict[str, object]) -> dict[str, object]:
    contract_delta_sha256 = "7" * 64
    excluded_paths = [SPECS_CONTRACT_EXCLUDED_PATH]
    return {
        "schema_version": "manyforge.specs_contract_identity.v1",
        "head_commit": specs["head_commit"],
        "head_tree": specs["head_tree"],
        "excluded_paths": excluded_paths,
        "contract_dirty": True,
        "contract_delta_sha256": contract_delta_sha256,
        "contract_fingerprint_sha256": specs_contract_fingerprint_sha256(
            head_commit=str(specs["head_commit"]),
            head_tree=str(specs["head_tree"]),
            excluded_paths=excluded_paths,
            contract_dirty=True,
            contract_delta_sha256=contract_delta_sha256,
        ),
    }


def _wrapper_command(input_path: Path) -> list[str]:
    source = _identity("1")
    specs = _identity("4")
    specs_contract = _contract_identity(specs)
    sha = "a" * 64
    wrapper_sha = hashlib.sha256(WRAPPER.read_bytes()).hexdigest()
    source_identity_sha = hashlib.sha256(
        (QUALIFICATION_ROOT / "source_identity.py").read_bytes()
    ).hexdigest()
    environment = {
        "requested_cpu_affinity": "10-13",
        "image_id": sha,
        "compiler": "13.3.0",
        "compiler_flags": "-Wno-error=maybe-uninitialized",
        "build_type": "Release",
        "build_jobs": 2,
        "cpu_governor_set": "performance",
        "cpu_frequency_driver_set": "intel_pstate",
        "kernel_id": "test-kernel",
        "source_root": "/source",
        "specs_root": "/specs",
        "source_identity_helper_sha256": source_identity_sha,
        "warmup_runs": 5,
        "measured_runs": 50,
    }
    return [
        sys.executable,
        str(WRAPPER),
        "--input",
        str(input_path),
        "--cell",
        "contact_grasp",
        "--run-id",
        "run-1",
        "--process-instance-id",
        "12345678-1234-4123-8123-123456789abc",
        "--source-role",
        "current_candidate",
        "--source-identity-json",
        json.dumps(source),
        "--specs-source-identity-json",
        json.dumps(specs),
        "--specs-contract-identity-json",
        json.dumps(specs_contract),
        "--expected-source-fingerprint-sha256",
        str(source["fingerprint_sha256"]),
        "--expected-specs-source-fingerprint-sha256",
        str(specs["fingerprint_sha256"]),
        "--expected-specs-contract-fingerprint-sha256",
        str(specs_contract["contract_fingerprint_sha256"]),
        "--image-id",
        sha,
        "--binary-sha256",
        "b" * 64,
        "--requested-cpu-affinity",
        "10-13",
        "--warmup-runs",
        "5",
        "--measured-runs",
        "50",
        "--checkout-base-clean",
        "0",
        "--runner-script-sha256",
        "c" * 64,
        "--wrapper-script-sha256",
        wrapper_sha,
        "--source-identity-helper-sha256",
        source_identity_sha,
        "--runner-command-json",
        '["runner","--cell","contact-grasp"]',
        "--build-command-json",
        '["docker","compose","run"]',
        "--outer-command-json",
        '["docker","compose","run"]',
        "--runner-environment-json",
        json.dumps(environment),
        "--",
        "taskset",
        "-c",
        "10-13",
        "/binary",
    ]


def test_wrapper_replaces_legacy_digests_with_exact_source_identities(
    tmp_path: Path,
) -> None:
    payload_path = tmp_path / "payload.json"
    payload_path.write_text(
        json.dumps(
            {
                "source_commit": "legacy",
                "specs_commit": "legacy",
                "specs_candidate_sha256": "legacy",
                "working_tree_candidate_sha256": "legacy",
                "warmup_runs": 5,
                "measured_runs": 50,
            }
        ),
        encoding="utf-8",
    )
    result = subprocess.run(
        _wrapper_command(payload_path),
        check=True,
        capture_output=True,
        text=True,
    )
    artifact = json.loads(result.stdout)
    assert artifact["schema_version"] == "manyforge.step18.proof_cell_artifact.v2"
    assert not (
        {
            "source_commit",
            "specs_commit",
            "specs_candidate_sha256",
            "working_tree_candidate_sha256",
        }
        & artifact["payload"].keys()
    )
    assert artifact["payload"]["source_identity"] == artifact["identity"][
        "source_identity"
    ]
    assert artifact["payload"]["specs_source_identity"] == artifact["identity"][
        "specs_source_identity"
    ]
    assert artifact["payload"]["specs_contract_identity"] == artifact["identity"][
        "specs_contract_identity"
    ]
    verifier = _load_verifier()
    checks = verifier.Checks()
    verifier.validate_envelope(artifact, checks)
    assert not checks.failures


def test_wrapper_rejects_nonformal_measurement_profile(tmp_path: Path) -> None:
    payload_path = tmp_path / "payload.json"
    payload_path.write_text("{}\n", encoding="utf-8")
    command = _wrapper_command(payload_path)
    measured_index = command.index("--measured-runs") + 1
    command[measured_index] = "49"
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode != 0
    assert "exactly 5 warmup and 50 measured" in result.stderr


def test_formal_verifier_refuses_policy_only_subgate(tmp_path: Path) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(VERIFIER),
            "--policy-support-only",
            "--baseline-clean-checkout-manifest",
            str(tmp_path / "manifest.json"),
            "--baseline-qualification-patch",
            str(tmp_path / "overlay.patch"),
            str(tmp_path / "artifact.json"),
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "requires --development-smoke" in result.stderr


def test_formal_verifier_requires_exactly_three_processes(tmp_path: Path) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(VERIFIER),
            "--minimum-runs",
            "2",
            "--baseline-clean-checkout-manifest",
            str(tmp_path / "manifest.json"),
            "--baseline-qualification-patch",
            str(tmp_path / "overlay.patch"),
            str(tmp_path / "artifact.json"),
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "must be exactly 3" in result.stderr


def test_formal_affinity_requires_exactly_four_canonical_cores() -> None:
    verifier = _load_verifier()
    assert verifier.four_core_affinity("10-13")
    assert verifier.four_core_affinity("1,3-5")
    assert not verifier.four_core_affinity("13")


def test_precise_reference_normalization_contract_is_closed_and_explicit() -> None:
    verifier = _load_verifier()
    contract = verifier._precise_normalization_contract()  # noqa: SLF001
    assert contract["legacy_baseline_diagnostic"] == {
        "field": "contact_release_min_separation_clearance_m",
        "required_baseline_value": 0,
        "required_baseline_serialized_value": "0.000000",
        "baseline_presence": "required_on_violation_records",
        "current_presence": "forbidden",
    }
    assert contract["missing_metric_classification_migration"] == {
        "case_id": "colliding_configured_metric_missing",
        "waypoint_index": 1,
        "pair": {
            "body_a": verifier.PRECISE_CONFIGURED_PAIR[0],
            "body_b": verifier.PRECISE_CONFIGURED_PAIR[1],
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
    }
    assert contract["volatile_result_fields"] == ["query_duration_ns"]
    assert verifier._precise_orchestration_projection_contract() == {  # noqa: SLF001
        "id": "step18_cross_worker_orchestration_projection.v1",
        "scope": "cross_worker_count_only",
        "excluded_result_fields": ["query_id"],
        "matched_source_profile_parity": "full_transcript_including_query_id",
    }


def test_precise_reference_diagnostic_normalization_rejects_widening() -> None:
    verifier = _load_verifier()
    baseline_audit: defaultdict[str, int] = defaultdict(int)
    baseline = verifier._precise_normalize_diagnostics(  # noqa: SLF001
        {
            "contact_release_violation": "missing_contact_release_clearance",
            "contact_release_min_separation_clearance_m": "0.000000",
        },
        verifier.BASELINE_ROLE,
        baseline_audit,
        "baseline",
    )
    assert baseline == {
        "contact_release_violation": "missing_contact_release_clearance",
        "contact_release_evidence_mode": "signed_penetration",
    }
    current_audit: defaultdict[str, int] = defaultdict(int)
    current = verifier._precise_normalize_diagnostics(  # noqa: SLF001
        {
            "contact_release_violation": "missing_contact_release_clearance",
            "contact_release_evidence_mode": "signed_penetration",
        },
        verifier.CURRENT_ROLE,
        current_audit,
        "current",
    )
    assert current == baseline
    for mutation, role in (
        (
            {
                "contact_release_violation": "missing_contact_release_clearance",
                "contact_release_min_separation_clearance_m": "0.000001",
            },
            verifier.BASELINE_ROLE,
        ),
        (
            {"contact_release_violation": "missing_contact_release_clearance"},
            verifier.BASELINE_ROLE,
        ),
        (
            {
                "contact_release_violation": "missing_contact_release_clearance",
                "contact_release_evidence_mode": "binary_contact",
            },
            verifier.CURRENT_ROLE,
        ),
        (
            {
                "contact_release_violation": "missing_contact_release_clearance",
                "contact_release_evidence_mode": "signed_penetration",
                "contact_release_min_separation_clearance_m": "0.000000",
            },
            verifier.CURRENT_ROLE,
        ),
    ):
        with pytest.raises(ValueError):
            verifier._precise_normalize_diagnostics(  # noqa: SLF001
                mutation, role, defaultdict(int), "mutated"
            )


def test_precise_reference_classification_migration_is_one_case_one_pair() -> None:
    verifier = _load_verifier()
    baseline_raw = [
        {
            "pair": list(verifier.PRECISE_CONFIGURED_PAIR),
            "classification_code": 3,
            "is_first_failure": True,
        }
    ]
    current_raw = deepcopy(baseline_raw)
    current_raw[0]["classification_code"] = 8
    baseline = verifier._precise_normalize_classification_migration(  # noqa: SLF001
        baseline_raw,
        verifier.BASELINE_ROLE,
        "colliding_configured_metric_missing",
        1,
        defaultdict(int),
        "baseline",
    )
    current = verifier._precise_normalize_classification_migration(  # noqa: SLF001
        current_raw,
        verifier.CURRENT_ROLE,
        "colliding_configured_metric_missing",
        1,
        defaultdict(int),
        "current",
    )
    assert baseline == current
    for mutation in (
        [{**baseline_raw[0], "classification_code": 2}],
        [{**baseline_raw[0], "is_first_failure": False}],
        [
            {
                **baseline_raw[0],
                "pair": list(verifier.PRECISE_UNCONFIGURED_PAIR),
            }
        ],
    ):
        with pytest.raises(ValueError, match="migration is not exact"):
            verifier._precise_normalize_classification_migration(  # noqa: SLF001
                mutation,
                verifier.BASELINE_ROLE,
                "colliding_configured_metric_missing",
                1,
                defaultdict(int),
                "mutated",
            )
    assert verifier._precise_normalize_classification_migration(  # noqa: SLF001
        baseline_raw,
        verifier.BASELINE_ROLE,
        "wrong_case",
        1,
        defaultdict(int),
        "wrong-case",
    ) == baseline_raw
    assert verifier._precise_normalize_classification_migration(  # noqa: SLF001
        baseline_raw,
        verifier.BASELINE_ROLE,
        "colliding_configured_metric_missing",
        2,
        defaultdict(int),
        "wrong-waypoint",
    ) == baseline_raw


def test_precise_reference_query_id_projection_is_orchestration_only() -> None:
    verifier = _load_verifier()
    expected = {
        1: ["validation_coordinator_w0", "validation_coordinator_w1", "validation_coordinator_w2", "validation_coordinator_w3", "validation_coordinator_w4"],
        2: ["validation_coordinator_w0", "validation_coordinator_w1", "validation_coordinator_w2", "validation_coordinator_w0", "validation_coordinator_w1"],
        3: ["validation_coordinator_w0", "validation_coordinator_w1", "validation_coordinator_w0", "validation_coordinator_w1", "validation_coordinator_w0"],
        4: ["validation_coordinator_w0", "validation_coordinator_w1", "validation_coordinator_w0", "validation_coordinator_w0", "validation_coordinator_w0"],
        5: ["validation_coordinator_w0"] * 5,
    }
    for workers, query_ids in expected.items():
        assert [
            verifier._precise_expected_query_id(index, 5, workers)  # noqa: SLF001
            for index in range(5)
        ] == query_ids
    raw = {
        "query_id": "validation_coordinator_w0",
        "engine_id": "step18_precise_reference_engine",
        "nested": [{"query_id": "validation_coordinator_w1", "decision": 1}],
    }
    assert verifier._precise_orchestration_projection(raw) == {  # noqa: SLF001
        "engine_id": "step18_precise_reference_engine",
        "nested": [{"decision": 1}],
    }


def test_precise_reference_first_failure_cardinality_is_decision_local() -> None:
    verifier = _load_verifier()
    pair = list(verifier.PRECISE_CONFIGURED_PAIR)
    decision = {"accepted": False, "first_failing_waypoint": 1}
    records = [
        {"waypoint_index": 0, "classified_pairs": []},
        {
            "waypoint_index": 1,
            "classified_pairs": [
                {"pair": pair, "is_first_failure": True}
            ],
        },
        {
            "waypoint_index": 2,
            "classified_pairs": [
                {"pair": pair, "is_first_failure": True}
            ],
        },
    ]
    verifier._precise_validate_first_failure(  # noqa: SLF001
        decision,
        records,
        "valid",
        require_semantic_marker=True,
        expected_pair=verifier.PRECISE_CONFIGURED_PAIR,
    )
    records[1]["classified_pairs"].append(
        {"pair": list(verifier.PRECISE_UNCONFIGURED_PAIR), "is_first_failure": True}
    )
    with pytest.raises(ValueError, match="exactly one pair"):
        verifier._precise_validate_first_failure(  # noqa: SLF001
            decision,
            records,
            "invalid",
            require_semantic_marker=True,
        )


def test_precise_reference_metadata_requires_exact_untimed_argv() -> None:
    verifier = _load_verifier()
    process_id = "12345678-1234-4123-8123-123456789abc"
    identity = {
        "source_role": verifier.CURRENT_ROLE,
        "image_id": "a" * 64,
        "binary_sha256": "b" * 64,
        "run_id": "run-current-1",
        "process_instance_id": process_id,
        "requested_cpu_affinity": "10-13",
    }
    payload = {
        "schema_version": verifier.PRECISE_REFERENCE_SCHEMA,
        "record_type": "precise_reference_parity",
        "passed": True,
        "timing_gate_excluded": True,
        "measurement_profile_usage": "provenance_only_no_timing_samples",
        "source_role": verifier.CURRENT_ROLE,
        "source_identity": {},
        "specs_source_identity": {},
        "specs_contract_identity": {},
        "image_id": identity["image_id"],
        "binary_sha256": identity["binary_sha256"],
        "run_id": identity["run_id"],
        "process_instance_id": process_id,
        "requested_cpu_affinity": "10-13",
        "effective_cpu_affinity": "10-13",
        "warmup_runs": 5,
        "measured_runs": 50,
        "build_type": "Release",
        "compiler_flags": "-Wno-error=maybe-uninitialized",
        "clean_checkout_manifest_sha256": None,
        "qualification_patch_sha256": None,
        "corpus_sha256": "c" * 64,
        "scheduler_policy": "SCHED_OTHER",
        "scheduler_priority": 0,
        "process_nice": 0,
        "kernel_id": "test-kernel",
        "cpu_governor_set": "performance",
        "cpu_frequency_driver_set": "test-driver",
        "compiler": "test-compiler",
        "reference_evidence": {},
    }
    provenance = {
        "clean_checkout_manifest_sha256": None,
        "qualification_patch_sha256": None,
        "command_argv": [
            "taskset",
            "-c",
            "10-13",
            "/build/perf_policy_support_contact_qualification",
            "--precise-reference-only",
            "--warmup-runs",
            "5",
            "--measured-runs",
            "50",
        ],
        "runner_command_argv": [
            "/repo/scripts/run-step18-proof-cell-qualification.sh",
            "--cell",
            "precise-reference-parity",
        ],
    }
    record = {
        "cell": "precise_reference_parity",
        "identity": identity,
        "provenance": provenance,
        "payload": payload,
    }
    checks = verifier.Checks()
    assert verifier.validate_precise_reference(record, checks) is None
    metadata = next(
        item
        for item in checks.items
        if item["name"].endswith(":precise_reference_metadata")
    )
    assert metadata["passed"] is True
    mutated = deepcopy(record)
    mutated["provenance"]["runner_command_argv"].extend(["--workers", "4"])
    checks = verifier.Checks()
    assert verifier.validate_precise_reference(mutated, checks) is None
    metadata = next(
        item
        for item in checks.items
        if item["name"].endswith(":precise_reference_metadata")
    )
    assert metadata["passed"] is False
    mutated = deepcopy(record)
    mutated["payload"]["foo_duration_ns"] = 1
    checks = verifier.Checks()
    assert verifier.validate_precise_reference(mutated, checks) is None
    metadata = next(
        item
        for item in checks.items
        if item["name"].endswith(":precise_reference_metadata")
    )
    assert metadata["passed"] is False
    assert not verifier.four_core_affinity("10,11,12,13")
    assert not verifier.four_core_affinity("10-14")


def test_proof_verifier_detects_post_snapshot_artifact_replacement(
    tmp_path: Path,
) -> None:
    verifier = _load_verifier()
    artifact = tmp_path / "proof.json"
    artifact.write_text('{"payload":"first"}\n', encoding="utf-8")
    record = verifier.load_artifact(artifact)
    artifact.write_text('{"payload":"second"}\n', encoding="utf-8")
    checks = verifier.Checks()
    verifier.require_artifacts_unchanged(
        [{"path": str(artifact), "sha256": record["_artifact"]["sha256"]}],
        checks,
    )
    assert len(checks.failures) == 1
    assert checks.failures[0]["name"] == "retained_input_unchanged:0"


def test_proof_verifier_output_cannot_create_candidate_source_file(
    tmp_path: Path,
) -> None:
    verifier = _load_verifier()
    repository = tmp_path / "manyforge"
    specs = tmp_path / "specs"
    repository.mkdir()
    specs.mkdir()
    subprocess.run(["git", "-C", str(repository), "init", "-q"], check=True)
    (repository / ".gitignore").write_text("qualification/\n", encoding="utf-8")
    with pytest.raises(ValueError, match="ignored qualification or build"):
        verifier.validated_evidence_output(
            repository / "new-source-file.json",
            repository,
            repository,
            {repository, specs},
        )
    with pytest.raises(ValueError, match="inside checkout"):
        verifier.validated_evidence_output(
            specs / "qualification" / "summary.json",
            repository,
            repository,
            {repository, specs},
        )
    with pytest.raises(ValueError, match="actually ignored"):
        verifier.validated_evidence_output(
            repository / "build" / "summary.json",
            repository,
            repository,
            {repository, specs},
        )
    assert verifier.validated_evidence_output(
        repository / "qualification" / "step18" / "summary.json",
        repository,
        repository,
        {repository, specs},
    ) == repository / "qualification" / "step18" / "summary.json"


def test_proof_summary_publication_discards_staging_on_retained_input_drift(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    verifier = _load_verifier()
    repository = tmp_path / "manyforge"
    repository.mkdir()
    subprocess.run(["git", "-C", str(repository), "init", "-q"], check=True)
    (repository / ".gitignore").write_text("qualification/\n", encoding="utf-8")
    artifact = tmp_path / "artifact.json"
    tool = tmp_path / "tool.py"
    artifact.write_text("artifact-v1\n", encoding="utf-8")
    tool.write_text("tool-v1\n", encoding="utf-8")
    artifact_manifest = [
        {
            "path": str(artifact),
            "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
        }
    ]
    tool_manifest = [
        {
            "path": str(tool),
            "sha256": hashlib.sha256(tool.read_bytes()).hexdigest(),
        }
    ]
    destination = repository / "qualification" / "step18" / "summary.json"
    original_stage = verifier.stage_output

    def stage_then_mutate(path: Path, payload: bytes) -> Path:
        staged = original_stage(path, payload)
        artifact.write_text("artifact-v2\n", encoding="utf-8")
        return staged

    monkeypatch.setattr(verifier, "stage_output", stage_then_mutate)
    with pytest.raises(ValueError, match="retained artifact changed"):
        verifier.publish_summary(
            destination,
            b'{"verification_passed":true}\n',
            [],
            repository,
            artifact_manifest,
            tool_manifest,
        )
    assert not destination.exists()
    assert not list(destination.parent.glob(".*.pending-*.tmp"))


def test_offline_wrappers_are_stdout_only() -> None:
    for wrapper in (
        WRAPPER,
        QUALIFICATION_ROOT / "wrap_runtime_collision_schedule_artifact.py",
    ):
        result = subprocess.run(
            [sys.executable, str(wrapper), "--help"],
            check=True,
            capture_output=True,
            text=True,
        )
        assert "--output" not in result.stdout
