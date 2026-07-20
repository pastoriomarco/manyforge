from __future__ import annotations

import importlib.util
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from types import ModuleType

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]


def load_module(relative: str, name: str) -> ModuleType:
    path = REPO_ROOT / relative
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_git(root: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(root), *args], check=True)


def test_specs_contract_identity_excludes_only_the_evidence_record(
    tmp_path: Path,
) -> None:
    identity = load_module(
        "scripts/qualification/source_identity.py", "step18_source_identity"
    )
    run_git(tmp_path, "init", "-q")
    run_git(tmp_path, "config", "user.email", "step18@example.invalid")
    run_git(tmp_path, "config", "user.name", "Step 18 Test")
    evidence = tmp_path / identity.SPECS_CONTRACT_EXCLUDED_PATH
    evidence.parent.mkdir(parents=True)
    evidence.write_text("initial evidence\n", encoding="utf-8")
    contract = tmp_path / "docs/spec/250-contact-policy.md"
    contract.parent.mkdir(parents=True)
    contract.write_text("contract v1\n", encoding="utf-8")
    run_git(tmp_path, "add", ".")
    run_git(tmp_path, "commit", "-qm", "baseline")

    initial_contract = identity.specs_contract_identity(tmp_path)
    initial_source = identity.source_identity(tmp_path)
    evidence.write_text("post-run evidence update\n", encoding="utf-8")
    assert identity.specs_contract_identity(tmp_path) == initial_contract
    assert identity.source_identity(tmp_path) != initial_source

    contract.write_text("contract v2\n", encoding="utf-8")
    assert identity.specs_contract_identity(tmp_path) != initial_contract


def test_shared_source_identity_matches_the_live_v1_authority() -> None:
    identity = load_module(
        "scripts/qualification/source_identity.py",
        "step18_shared_source_identity",
    )
    live = load_module(
        "scripts/qualification/roboplan_05_qualification.py",
        "step18_live_source_identity",
    )
    assert identity.source_identity(REPO_ROOT) == live._manyforge_source_identity(  # noqa: SLF001
        REPO_ROOT
    )


def test_runtime_verifier_rejects_fewer_than_three_formal_runs() -> None:
    verifier = REPO_ROOT / "scripts/qualification/verify_runtime_collision_schedule_step18.py"
    result = subprocess.run(
        [
            sys.executable,
            str(verifier),
            "--minimum-runs",
            "1",
            "/does/not/exist.json",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "formal --minimum-runs must be exactly 3" in result.stderr


def test_runtime_group_requires_unique_process_and_artifact_identity() -> None:
    verifier = load_module(
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
        "step18_runtime_verifier",
    )
    group = [
        {
            "identity": {
                "run_id": f"run-{index}",
                "process_instance_id": f"process-{index}",
            },
            "_artifact": {
                "path": f"/evidence/run-{index}.json",
                "sha256": f"hash-{index}",
            },
        }
        for index in range(3)
    ]
    assert verifier.independent_process_group(group, 3)[0] is True
    group[2]["identity"]["process_instance_id"] = "process-1"
    assert verifier.independent_process_group(group, 3)[0] is False
    group[2]["identity"]["process_instance_id"] = "process-2"
    group[2]["_artifact"]["sha256"] = "hash-1"
    assert verifier.independent_process_group(group, 3)[0] is False
    group[2]["_artifact"]["sha256"] = "hash-2"
    group.append(
        {
            "identity": {
                "run_id": "run-4",
                "process_instance_id": "process-4",
            },
            "_artifact": {
                "path": "/evidence/run-4.json",
                "sha256": "hash-4",
            },
        }
    )
    assert verifier.independent_process_group(group, 3)[0] is False


def test_runtime_verifier_detects_post_snapshot_artifact_replacement(
    tmp_path: Path,
) -> None:
    verifier = load_module(
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
        "step18_runtime_snapshot_verifier",
    )
    artifact = tmp_path / "runtime.json"
    artifact.write_text('{"payload":"first"}\n', encoding="utf-8")
    records = verifier.load_artifact(artifact)
    artifact.write_text('{"payload":"second"}\n', encoding="utf-8")
    checks = verifier.Checks()
    verifier.require_artifacts_unchanged(
        [{"path": str(artifact), "sha256": records[0]["_artifact"]["sha256"]}],
        checks,
    )
    assert len(checks.failures) == 1
    assert checks.failures[0]["name"] == "retained_input_unchanged:0"


def test_runtime_summary_publication_discards_staging_on_parity_probe_drift(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    verifier = load_module(
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
        "step18_runtime_publication_verifier",
    )
    repository = tmp_path / "manyforge"
    repository.mkdir()
    run_git(repository, "init", "-q")
    (repository / ".gitignore").write_text("qualification/\n", encoding="utf-8")
    artifact = tmp_path / "artifact.json"
    probe = tmp_path / "probe.json"
    tool = tmp_path / "tool.py"
    artifact.write_text("artifact-v1\n", encoding="utf-8")
    probe.write_text("probe-v1\n", encoding="utf-8")
    tool.write_text("tool-v1\n", encoding="utf-8")

    def manifest(path: Path) -> list[dict[str, str]]:
        return [
            {
                "path": str(path),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        ]

    destination = repository / "qualification" / "step18" / "summary.json"
    original_stage = verifier.stage_output

    def stage_then_mutate(path: Path, payload: bytes) -> Path:
        staged = original_stage(path, payload)
        probe.write_text("probe-v2\n", encoding="utf-8")
        return staged

    monkeypatch.setattr(verifier, "stage_output", stage_then_mutate)
    with pytest.raises(ValueError, match="retained parity probe changed"):
        verifier.publish_summary(
            destination,
            b'{"runtime_schedule_subgate_passed":true}\n',
            [],
            repository,
            manifest(artifact),
            manifest(probe),
            manifest(tool),
        )
    assert not destination.exists()
    assert not list(destination.parent.glob(".*.pending-*.tmp"))


def test_runtime_verifier_rejects_multi_record_formal_input(
    tmp_path: Path,
) -> None:
    artifact = tmp_path / "records.json"
    artifact.write_text("[{}, {}]\n", encoding="utf-8")
    verifier = (
        REPO_ROOT
        / "scripts/qualification/verify_runtime_collision_schedule_step18.py"
    )
    result = subprocess.run(
        [sys.executable, str(verifier), str(artifact)],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2
    assert "must contain exactly one record" in result.stderr


def test_runtime_record_rejects_a_reduced_formal_sampling_profile() -> None:
    verifier = load_module(
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
        "step18_runtime_profile_verifier",
    )
    record = {
        "_artifact": {"path": "reduced.json", "record_index": 0},
        "identity": {},
        "inputs": {"bounded_threshold_m": 2.0},
        "config": {**verifier.FORMAL_PROFILE, "measured_ticks_requested": 1},
    }
    checks = verifier.Checks()
    verifier.validate_record(record, checks)
    profile_checks = [
        item
        for item in checks.items
        if item["name"].endswith(":exact_formal_sampling_profile")
    ]
    assert profile_checks == [
        {
            "name": "reduced.json#0:exact_formal_sampling_profile",
            "passed": False,
            "detail": "",
        }
    ]


def test_runtime_offline_sources_have_no_legacy_worktree_digest_clone() -> None:
    paths = (
        "scripts/prepare-runtime-collision-schedule-baseline.sh",
        "scripts/run-runtime-collision-schedule-baseline.sh",
        "scripts/run-runtime-collision-schedule-qualification.sh",
    )
    for relative in paths:
        text = (REPO_ROOT / relative).read_text(encoding="utf-8")
        assert "working_tree_candidate_sha256" not in text
        assert "git -C \"${REPO_ROOT}\" diff --binary HEAD" not in text
        assert "scripts/qualification/source_identity.py" in text


def test_runtime_artifact_schema_exposes_no_overall_step18_close() -> None:
    verifier = (
        REPO_ROOT
        / "scripts/qualification/verify_runtime_collision_schedule_step18.py"
    ).read_text(encoding="utf-8")
    harness = (
        REPO_ROOT
        / "tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp"
    ).read_text(encoding="utf-8")
    assert '"step18_closed"' not in verifier
    assert '"requires_common_candidate_aggregator": True' in verifier
    assert "working_tree_candidate_sha256" not in harness
    assert "manyforge.runtime_collision_schedule_qualification.v2" in harness


def test_runtime_wrapper_rejects_host_native_process_mismatch(
    tmp_path: Path,
) -> None:
    wrapper = (
        REPO_ROOT
        / "scripts/qualification/wrap_runtime_collision_schedule_artifact.py"
    )
    identity_authority = load_module(
        "scripts/qualification/source_identity.py",
        "step18_runtime_wrapper_identity",
    )
    checkout = tmp_path / "checkout"
    checkout.mkdir()
    run_git(checkout, "init", "-q")
    run_git(checkout, "config", "user.email", "step18@example.invalid")
    run_git(checkout, "config", "user.name", "Step 18 Test")
    (checkout / "input.txt").write_text("identity\n", encoding="utf-8")
    run_git(checkout, "add", ".")
    run_git(checkout, "commit", "-qm", "identity")
    source = identity_authority.source_identity(checkout)
    contract = identity_authority.specs_contract_identity(checkout)
    artifact = tmp_path / "native.json"
    artifact.write_text(
        json.dumps(
            {
                "schema_version": (
                    "manyforge.runtime_collision_schedule_qualification.v2"
                ),
                "identity": {
                    "process_instance_id": "native-process",
                    "source_identity": source,
                    "specs_source_identity": source,
                    "specs_contract_identity": contract,
                },
                "provenance": {},
            }
        ),
        encoding="utf-8",
    )
    result = subprocess.run(
        [
            sys.executable,
            str(wrapper),
            "--input",
            str(artifact),
            "--process-instance-id",
            "host-process",
            "--source-identity-json",
            json.dumps(source),
            "--specs-source-identity-json",
            json.dumps(source),
            "--specs-contract-identity-json",
            json.dumps(contract),
            "--runner-command-json",
            '["runner"]',
            "--build-commands-json",
            '[["build"]]',
            "--outer-command-json",
            '["docker","compose","run"]',
            "--runner-environment-json",
            '{"fixed":"environment"}',
            "--runner-script-sha256",
            "0" * 64,
            "--source-identity-script-sha256",
            "1" * 64,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "native process instance does not match" in result.stderr


def test_runtime_parity_generator_rejects_a_formal_record_as_a_probe() -> None:
    generator = load_module(
        "scripts/qualification/generate_runtime_collision_baseline_parity.py",
        "step18_runtime_parity_generator",
    )
    record = {
        "schema_version": generator.RECORD_SCHEMA,
        "identity": {
            "measurement_kind": "timing",
            "run_id": "formal-run",
            "process_instance_id": "5e7f1f7e-fc10-4d36-b412-cce396291647",
            "source_identity": {"head_commit": generator.BASELINE_SOURCE},
            "specs_source_identity": {"head_commit": generator.BASELINE_SPECS},
            "specs_contract_identity": {
                "head_commit": generator.BASELINE_SPECS,
                "contract_dirty": False,
            },
        },
        "inputs": {"arm": "current_all_active_distance"},
        "provenance": {
            "source_role": "pristine_baseline_timing",
            "runner_environment": {"purpose": "formal"},
        },
    }
    with pytest.raises(ValueError, match="not labeled as a parity probe"):
        generator.validate_identity(
            record,
            "timing",
            "pristine_baseline_timing",
        )


def test_runtime_parity_output_cannot_enter_qualification_checkouts(
    tmp_path: Path,
) -> None:
    generator = load_module(
        "scripts/qualification/generate_runtime_collision_baseline_parity.py",
        "step18_runtime_parity_output",
    )
    timing_source = tmp_path / "timing-source"
    specs_source = tmp_path / "specs-source"
    timing_source.mkdir()
    specs_source.mkdir()
    records = (
        {
            "provenance": {
                "runner_environment": {
                    "source_root": str(timing_source),
                    "specs_root": str(specs_source),
                }
            }
        },
        {
            "provenance": {
                "runner_environment": {
                    "source_root": str(timing_source),
                    "specs_root": str(specs_source),
                }
            }
        },
    )
    with pytest.raises(ValueError, match="inside qualification checkout"):
        generator.validated_output(timing_source / "parity.json", records)
    with pytest.raises(ValueError, match="inside qualification checkout"):
        generator.validated_output(specs_source / "parity.json", records)
    with pytest.raises(ValueError, match="ignored qualification or build"):
        generator.validated_output(REPO_ROOT / "parity.json", records)
    assert generator.validated_output(
        REPO_ROOT / "qualification" / "step18" / "parity.json", records
    ) == REPO_ROOT / "qualification" / "step18" / "parity.json"


def test_runtime_parity_atomic_publish_never_overwrites(
    tmp_path: Path,
) -> None:
    generator = load_module(
        "scripts/qualification/generate_runtime_collision_baseline_parity.py",
        "step18_runtime_parity_atomic_publish",
    )
    destination = tmp_path / "parity.json"
    destination.write_text("existing\n", encoding="utf-8")
    staged = generator.stage_output(destination, b"replacement\n")
    with pytest.raises(ValueError, match="refusing to overwrite"):
        generator.publish_staged_exclusive(staged, destination)
    assert destination.read_text(encoding="utf-8") == "existing\n"
    assert not staged.exists()
