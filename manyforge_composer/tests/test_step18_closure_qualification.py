from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_QUALIFICATION = _REPO_ROOT / "scripts/qualification"


def _load_module(name: str, path: Path):  # type: ignore[no-untyped-def]
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


_SOURCE = sys.modules.get("source_identity") or _load_module(
    "source_identity", _QUALIFICATION / "source_identity.py"
)
_GENERATE = _load_module(
    "generate_step18_closure_manifest",
    _QUALIFICATION / "generate_step18_closure_manifest.py",
)
_VERIFY = _load_module(
    "verify_step18_closure", _QUALIFICATION / "verify_step18_closure.py"
)


def _write(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def _json_write(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def _json_read(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    assert isinstance(value, dict)
    return value


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


_DISTANCE_CAPABILITY_SOURCES = (
    "manyforge_core/include/manyforge_core/types/collision.hpp",
    "manyforge_core/include/manyforge_core/collision/i_collision_query_engine.hpp",
    "manyforge_collision/src/manyforge_collision_engine.cpp",
    "manyforge_core/src/collision_validation_coordinator.cpp",
    "manyforge_core/src/runtime_collision_query_session.cpp",
)


def _copy_distance_capability_sources(destination: Path) -> Path:
    for relative in _DISTANCE_CAPABILITY_SOURCES:
        _write(destination / relative, (_REPO_ROOT / relative).read_text(encoding="utf-8"))
    return destination


def _git(repo: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def _commit_repo(repo: Path, files: dict[str, str]) -> None:
    repo.mkdir(parents=True)
    _git(repo, "init", "-q")
    _git(repo, "config", "user.email", "qualification@example.invalid")
    _git(repo, "config", "user.name", "Qualification Test")
    for relative, value in files.items():
        _write(repo / relative, value)
    _git(repo, "add", "--all")
    _git(repo, "commit", "-qm", "fixture")


def _artifact(path: Path, value: str = "artifact\n") -> dict[str, str]:
    _write(path, value)
    return {"path": str(path.resolve()), "sha256": _sha256(path)}


def _evidence_record(
    summary_paths: dict[str, Path],
    closure_verifier: Path,
    prose: str = "fixture\n",
) -> str:
    claim = {
        "schema_version": _GENERATE.EVIDENCE_SCHEMA,
        "status": "ready_for_common_candidate_closure",
        "overall_step18_closure_claimed": False,
        "closure_verifier": {
            "path": "scripts/qualification/verify_step18_closure.py",
            "sha256": _sha256(closure_verifier),
        },
        "subgates": {
            name: {"summary_sha256": _sha256(summary_paths[name]), "passed": True}
            for name in _GENERATE.INPUTS
        },
    }
    return (
        prose
        + _GENERATE.EVIDENCE_BEGIN
        + "\n"
        + json.dumps(claim, indent=2, sort_keys=True)
        + "\n"
        + _GENERATE.EVIDENCE_END
        + "\n"
    )


def _fixture(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> tuple[Path, dict[str, Any]]:
    manyforge = tmp_path / "manyforge"
    specs = tmp_path / "specs"
    engine = """
void publish_capability() {
  capability_.supported_distance_request_modes = {
      CollisionDistanceRequestMode::kNone,
      CollisionDistanceRequestMode::kSelectedCollidingPairs,
      CollisionDistanceRequestMode::kAllCollidingPairs,
      CollisionDistanceRequestMode::kAllActivePairs};
}
void diagnostic_query() {
  if (request == kAllActivePairs) {
    computeDistances(model, data);
  }
  if (runtime_request == kAllActivePairs) {
    computeDistances(model, runtime_data);
  }
  computeDistance(model, pair_one);
  computeDistance(model, pair_two);
}
"""
    committed_manyforge = {
        ".gitignore": ".manyforge-var/\nbuild/\n",
        "manyforge_collision/src/manyforge_collision_engine.cpp": engine,
        "manyforge_core/include/manyforge_core/types/collision.hpp": """
struct CollisionEngineCapabilityDescriptor {
  std::vector<CollisionDistanceRequestMode> supported_distance_request_modes;
  bool supports_distance_query;
};
inline bool collision_distance_request_modes_valid(
    const CollisionEngineCapabilityDescriptor& capability) {
  const auto& modes = capability.supported_distance_request_modes;
  if (modes.empty()) return false;
  for (std::size_t index = 1; index < modes.size(); ++index) {
    if (static_cast<int>(modes[index - 1]) >= static_cast<int>(modes[index])) return false;
  }
  if (!capability.supports_distance_query) return false;
  return true;
}
inline bool collision_engine_supports_distance_request_mode(
    const CollisionEngineCapabilityDescriptor& capability,
    CollisionDistanceRequestMode requested_mode) {
  if (!collision_distance_request_modes_valid(capability)) return false;
  return std::binary_search(
      capability.supported_distance_request_modes.begin(),
      capability.supported_distance_request_modes.end(), requested_mode);
}
/// Compare-mode policy
""",
        "manyforge_core/include/manyforge_core/collision/i_collision_query_engine.hpp": """
auto runtime_collision_query_capability_status(const auto& capability) {
  if (!collision_distance_request_modes_valid(capability)) return invalid;
  return admitted;
}
const char* runtime_collision_query_capability_status_name(int status) { return "fixture"; }
""",
        "manyforge_core/src/collision_validation_coordinator.cpp": """
std::vector<std::string> required_capabilities_for(
    const auto& snapshot, const std::string& policy,
    CollisionDistanceRequestMode distance_request_mode) {
  std::vector<std::string> capabilities;
  if (distance_request_mode != CollisionDistanceRequestMode::kNone) {
    capabilities.push_back("penetration_depth");
  }
  return capabilities;
}
bool engine_admits_query_capabilities(
    const auto& capability, CollisionDistanceRequestMode distance_request_mode,
    const auto& required_capabilities) {
  if (!collision_engine_supports_distance_request_mode(
          capability, distance_request_mode)) return false;
  return true;
}
CollisionResult capability_admission_failure() { return {}; }
void query_one() { engine_admits_query_capabilities(capability, mode, required); }
void query_two() { engine_admits_query_capabilities(capability, mode, required); }
""",
        "manyforge_core/src/runtime_collision_query_session.cpp": """
bool RuntimeCollisionQuerySession::bind_distance_selection(const auto& bundle) noexcept {
  if (!collision_engine_supports_distance_request_mode(*capability_,
                                                       requested_mode)) return false;
  return true;
}
bool RuntimeCollisionQuerySession::bind_policy(const auto& bundle) noexcept { return true; }
""",
    }
    for relative_verifier, _ in _GENERATE.PRODUCERS.values():
        committed_manyforge[relative_verifier] = (
            "#!/usr/bin/env python3\n"
            "raise SystemExit('fixture verifier must be replay-stubbed')\n"
        )
    committed_manyforge["scripts/qualification/run_step18_release_gates.py"] = (
        "#!/usr/bin/env python3\n# fixture release runner\n"
    )
    _commit_repo(manyforge, committed_manyforge)
    _commit_repo(specs, {"docs/spec/250-contact-policy.md": "Step 18 contract\n"})
    _write(manyforge / "candidate-delta.txt", "candidate\n")
    _write(specs / "docs/spec/step18-delta.md", "candidate contract\n")

    source = _SOURCE.source_identity(manyforge)
    campaign_specs = _SOURCE.source_identity(specs)
    contract = _SOURCE.specs_contract_identity(specs)
    monkeypatch.setattr(_VERIFY, "PINNED_BASELINE_COMMIT", source["head_commit"])
    monkeypatch.setattr(_VERIFY, "PINNED_BASELINE_TREE", source["head_tree"])
    monkeypatch.setattr(_VERIFY, "PINNED_SPECS_COMMIT", campaign_specs["head_commit"])
    monkeypatch.setattr(_VERIFY, "PINNED_SPECS_TREE", campaign_specs["head_tree"])

    evidence_root = manyforge / ".manyforge-var/qualification/closure-fixture"
    artifacts = evidence_root / "artifacts"

    proof_manifest = [
        {
            "role": "proof_cell_artifact",
            **_artifact(artifacts / f"proof-{index:03d}.json", f"proof {index}\n"),
            "cell": "policy_support_contact",
            "source_role": "candidate",
            "run_id": f"proof-run-{index:03d}",
            "process_instance_id": f"proof-process-{index:03d}",
            "arm": "precise",
            "waypoints": 96,
            "workers": 1,
        }
        for index in range(138)
    ]
    for source_offset, source_role in enumerate(
        ("pristine_baseline_timing", "current_candidate")
    ):
        for repetition in range(3):
            index = 138 + source_offset * 3 + repetition
            proof_manifest.append(
                {
                    "role": "proof_cell_artifact",
                    **_artifact(
                        artifacts / f"proof-{index:03d}.json",
                        f"proof {index}\n",
                    ),
                    "cell": "precise_reference_parity",
                    "source_role": source_role,
                    "run_id": f"precise-reference-{source_role}-{repetition}",
                    "process_instance_id": (
                        f"precise-reference-process-{source_role}-{repetition}"
                    ),
                    "arm": None,
                    "waypoints": None,
                    "workers": None,
                }
            )
    proof_manifest.extend(
        (
            {
                "role": "baseline_clean_checkout_manifest",
                **_artifact(artifacts / "proof-clean-checkout.json"),
            },
            {
                "role": "baseline_qualification_patch",
                **_artifact(artifacts / "proof-qualification.patch"),
            },
        )
    )
    proof_tool_roles = (
        "proof_verifier",
        "proof_wrapper",
        "source_identity_helper",
        "current_proof_runner",
        "baseline_proof_runner",
        "baseline_prepare_runner",
        "baseline_policy_compat_patch",
        "baseline_free_space_profile_patch",
        "baseline_free_space_query_profile_patch",
    )
    proof_tools = [
        {
            "role": role,
            **_artifact(artifacts / f"proof-tool-{role}.txt", role + "\n"),
        }
        for role in proof_tool_roles
    ]
    small_profile_sha256 = {
        str(worker): _digest(f"small-profile-{worker}")
        for worker in range(1, 6)
    }
    small_case_sha256 = {
        str(worker): {
            case: _digest(f"small-case-{worker}-{case}")
            for case in _VERIFY.PRECISE_REFERENCE_CASE_ORDER
        }
        for worker in range(1, 6)
    }
    partition_profile_sha256 = {
        str(worker): _digest(f"partition-profile-{worker}")
        for worker in range(1, 6)
    }
    completion_profile_keys = (
        "serial",
        *(
            "-".join(map(str, order))
            for order in _VERIFY.PRECISE_REFERENCE_COMPLETION_ORDERS
        ),
    )
    completion_order_sha256 = {
        key: _digest(f"completion-profile-{key}")
        for key in completion_profile_keys
    }
    exceptional_case_sha256 = {
        name: _digest(f"exceptional-{name}")
        for name in (
            "pre_cancelled",
            "expired_absolute_deadline",
            "worker_exception",
        )
    }
    normalized_transcript_sha256 = _digest("normalized-transcript")
    input_corpus_sha256 = _digest("input-corpus")
    precise_manifest = proof_manifest[138:144]
    per_record_transcripts = [
        {
            "source_role": artifact["source_role"],
            "repetition": index % 3 + 1,
            "run_id": artifact["run_id"],
            "process_instance_id": artifact["process_instance_id"],
            "artifact_sha256": artifact["sha256"],
            "input_corpus_sha256": input_corpus_sha256,
            "normalized_transcript_sha256": normalized_transcript_sha256,
            "small_case_profile_sha256": copy.deepcopy(small_profile_sha256),
            "small_case_sha256": copy.deepcopy(small_case_sha256),
            "partition_profile_sha256": copy.deepcopy(
                partition_profile_sha256
            ),
            "completion_order_sha256": copy.deepcopy(
                completion_order_sha256
            ),
            "exceptional_case_sha256": copy.deepcopy(
                exceptional_case_sha256
            ),
        }
        for index, artifact in enumerate(precise_manifest)
    ]
    audit_fields = {
        "baseline_legacy_zero_observations": 0,
        "baseline_implied_signed_mode_observations": 0,
        "current_explicit_signed_mode_observations": 0,
        "baseline_raw_mode_absent_profiles": 0,
        "current_raw_mode_present_profiles": 0,
        "baseline_missing_metric_code3_observations": 0,
        "current_missing_metric_code8_observations": 0,
        "omitted_global_volatile_diagnostic_values": 0,
    }
    baseline_audit = {
        **audit_fields,
        "baseline_legacy_zero_observations": 25,
        "baseline_implied_signed_mode_observations": 25,
        "baseline_raw_mode_absent_profiles": 50,
        "baseline_missing_metric_code3_observations": 5,
    }
    current_audit = {
        **audit_fields,
        "current_explicit_signed_mode_observations": 25,
        "current_raw_mode_present_profiles": 50,
        "current_missing_metric_code8_observations": 5,
    }
    proof = {
        "schema_version": _VERIFY.PROOF_SCHEMA,
        "verification_scope": "offline_proof_cells",
        "minimum_runs": 3,
        "development_smoke": False,
        "offline_proof_gate_passed": True,
        "verification_passed": True,
        "failure_count": 0,
        "record_count": 144,
        "overall_step18_closure_claimed": False,
        "requires_common_candidate_aggregator": True,
        "candidate_source_identity": source,
        "candidate_specs_source_identity": campaign_specs,
        "candidate_specs_contract_identity": contract,
        "artifact_manifest": proof_manifest,
        "tool_manifest": proof_tools,
        "performance": {"all_required_gates": "passed"},
        "precise_reference_parity": {
            "timing_gate_excluded": True,
            "record_count": 6,
            "runs_per_source_role": 3,
            "case_count": 10,
            "case_order": list(_VERIFY.PRECISE_REFERENCE_CASE_ORDER),
            "small_case_worker_counts": [1, 2, 3, 4, 5],
            "normalization_contract_id": (
                "step18_precise_reference_normalization.v1"
            ),
            "normalization_contract_sha256": _digest(
                "normalization-contract"
            ),
            "orchestration_projection_contract_id": (
                "step18_cross_worker_orchestration_projection.v1"
            ),
            "orchestration_projection_contract_sha256": _digest(
                "orchestration-projection-contract"
            ),
            "input_corpus_sha256": input_corpus_sha256,
            "baseline_transcript_sha256": normalized_transcript_sha256,
            "current_transcript_sha256": normalized_transcript_sha256,
            "semantic_parity": True,
            "matched_profile_full_stable_parity": True,
            "cross_worker_orchestration_projection": {
                "excluded_result_fields": ["query_id"],
                "small_case_profile_sha256": {
                    str(worker): _digest("projected-small-profile")
                    for worker in range(1, 6)
                },
                "partition_profile_sha256": {
                    str(worker): _digest("projected-partition-profile")
                    for worker in range(1, 6)
                },
                "completion_sha256": {
                    key: _digest("projected-completion-profile")
                    for key in completion_profile_keys
                },
                "parity": True,
            },
            "per_record_transcripts": per_record_transcripts,
            "normalization_audit": {
                "pristine_baseline_timing": baseline_audit,
                "current_candidate": current_audit,
            },
            "partition_inventory": {
                "sample_kind_count": 4,
                "waypoint_count": 5,
                "patterns_per_worker": 1024,
                "worker_counts": [2, 3, 4, 5],
                "total_partition_evaluations_per_record": 4096,
                "serial_reference_evaluations_per_record": 1024,
                "matched_profile_sha256": copy.deepcopy(
                    partition_profile_sha256
                ),
                "parity": True,
            },
            "completion_order_inventory": {
                "workers": 3,
                "permutation_count": 6,
                "orders": [
                    list(order)
                    for order in _VERIFY.PRECISE_REFERENCE_COMPLETION_ORDERS
                ],
                "matched_profile_sha256": copy.deepcopy(
                    completion_order_sha256
                ),
                "parity": True,
            },
            "exceptional_cases": {
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
                    "earliest": {
                        "waypoint": 4,
                        "time_ns": 40_000_000,
                    },
                    "parity": True,
                },
            },
        },
        "checks": [{"name": "proof-complete", "passed": True, "detail": ""}],
    }

    runtime_records = [
        {
            "role": "runtime_schedule_artifact",
            **_artifact(
                artifacts / f"runtime-{index:02d}.json", f"runtime {index}\n"
            ),
            "arm": "precise",
            "measurement_kind": "timing",
            "run_id": f"runtime-run-{index:02d}",
            "process_instance_id": f"runtime-process-{index:02d}",
        }
        for index in range(24)
    ]
    runtime_auxiliaries = []
    for role in (
        "baseline_clean_checkout_manifest",
        "baseline_harness_patch",
        "baseline_observation_patch",
        "baseline_decision_parity_artifact",
    ):
        runtime_auxiliaries.append(
            {
                "role": role,
                **_artifact(artifacts / f"runtime-{role}.json", role + "\n"),
            }
        )
    parity_artifact = runtime_auxiliaries[-1]
    runtime_probe_roles = (
        "baseline_timing_parity_probe",
        "baseline_observation_parity_probe",
    )
    runtime_probes = [
        {
            "role": role,
            **_artifact(artifacts / f"runtime-probe-{role}.json", role + "\n"),
        }
        for role in runtime_probe_roles
    ]
    runtime_tool_roles = (
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
    )
    runtime_tools = [
        {
            "role": role,
            **_artifact(artifacts / f"runtime-tool-{role}.txt", role + "\n"),
        }
        for role in runtime_tool_roles
    ]
    runtime = {
        "schema_version": _VERIFY.RUNTIME_SCHEMA,
        "record_count": 24,
        "minimum_runs": 3,
        "formal_profile": {"workers": 2, "runtime_frequency_hz": 20},
        "formal_bounded_threshold_m": 0.002,
        "baseline_required": True,
        "genuine_pristine_baseline_present": True,
        "before_after_measured": True,
        "available_checks_passed": True,
        "runtime_schedule_subgate_passed": True,
        "failure_count": 0,
        "overall_step18_closure_claimed": False,
        "requires_common_candidate_aggregator": True,
        "candidate_source_identity": source,
        "candidate_specs_source_identity": campaign_specs,
        "candidate_specs_contract_identity": contract,
        "artifact_manifest": [*runtime_records, *runtime_auxiliaries],
        "parity_probe_manifest": runtime_probes,
        "tool_manifest": runtime_tools,
        "baseline_provenance_hashes": {
            "baseline_decision_parity_artifact": parity_artifact["sha256"]
        },
        "checks": [
            {"name": "runtime-complete", "passed": True, "detail": ""},
            {
                "name": "baseline_decision_parity:typed_claims",
                "passed": True,
                "detail": "",
            },
            {
                "name": (
                    "baseline_decision_parity:"
                    "artifact_binds_normalized_pair_outputs"
                ),
                "passed": True,
                "detail": "",
            },
            {
                "name": "baseline_decision_parity:recomputed_status_reason_exact32",
                "passed": True,
                "detail": "",
            },
        ],
    }

    live_manifest_binding = _artifact(artifacts / "live-manifest.json")
    live_verifier_path = (
        manyforge / _GENERATE.PRODUCERS["live_contact_modes"][0]
    ).resolve()
    live_verifier_binding = {
        "path": str(live_verifier_path),
        "sha256": _sha256(live_verifier_path),
    }
    live_patch_binding = _artifact(artifacts / "live-observation.patch")
    live_parity_binding = _artifact(artifacts / "live-parity.json")
    live_runner_binding = _artifact(artifacts / "live-row-runner.py")
    live_rows: list[dict[str, Any]] = []
    live_row_artifacts: list[dict[str, Any]] = []
    for index in range(16):
        row_id = f"row-{index}"
        evidence_binding = _artifact(artifacts / f"live-{row_id}-evidence.json")
        log_binding = _artifact(artifacts / f"live-{row_id}-controller.log")
        metadata_binding = _artifact(artifacts / f"live-{row_id}-metadata.json")
        live_rows.append(
            {
                "id": row_id,
                "evidence": evidence_binding,
                "controller_log": log_binding,
                "run_metadata": metadata_binding,
            }
        )
        live_row_artifacts.extend(
            (
                {"role": "live_row_evidence", "row_id": row_id, **evidence_binding},
                {
                    "role": "live_row_controller_log",
                    "row_id": row_id,
                    **log_binding,
                },
                {
                    "role": "live_row_metadata",
                    "row_id": row_id,
                    **metadata_binding,
                },
            )
        )
    live_artifact_manifest = [
        {"role": "live_manifest", **live_manifest_binding},
        {"role": "live_verifier", **live_verifier_binding},
        {"role": "baseline_observation_patch", **live_patch_binding},
        {"role": "baseline_observation_parity", **live_parity_binding},
        {"role": "live_row_runner", **live_runner_binding},
        *live_row_artifacts,
    ]
    live_mechanism_bindings = [
        _artifact(artifacts / f"live-mechanism-{index:02d}.json")
        for index in range(43)
    ]
    live_artifact_manifest.extend(
        {"role": "live_mechanism_replay_input", **binding}
        for binding in live_mechanism_bindings
    )
    live = {
        "schema_version": _VERIFY.LIVE_SCHEMA,
        "scope": "step18_live_validation_nonregression_subverification",
        "live_lane_passed": True,
        "overall_step18_closure_claimed": False,
        "failures": [],
        "rows": live_rows,
        "comparisons": [{"passed": True} for _ in range(6)],
        "pinned_baseline_commit": source["head_commit"],
        "pinned_specs_commit": campaign_specs["head_commit"],
        "candidate_source_identity": source,
        "candidate_specs_source_identity": campaign_specs,
        "candidate_specs_contract_identity": contract,
        "manifest": live_manifest_binding,
        "verifier": live_verifier_binding,
        "baseline_observation_patch": live_patch_binding,
        "artifact_manifest": live_artifact_manifest,
        "producer_invocation": {
            "schema_version": "manyforge.step18.producer_invocation.v1",
            "verifier": live_verifier_binding,
            "arguments": ["--manifest", live_manifest_binding["path"]],
            "inputs": [{"role": "manifest", **live_manifest_binding}],
            "result_transport": "output_json",
        },
        "baseline_observation_parity": {
            "scope": "retained_live_decision_trajectory_outputs",
        },
        "live_mechanism_replay": {
            "manifest": live_mechanism_bindings[0],
            "runner": live_mechanism_bindings[1],
            "baseline_lane_metadata": live_mechanism_bindings[2],
            "owner_scope": "exact_ur10e_isaac_live_validation_mechanism_owner",
            "observation_row_count": 8,
            "records": [
                {
                    "observation_row_id": f"row-{index}",
                    "direct_mechanism": {"candidate_observation": {"query_count": 96}},
                    "physical_pair_parity_passed": True,
                    "observation_nonperturbation_passed": True,
                }
                for index in range(8)
            ],
            "retained_artifacts": live_mechanism_bindings,
            "all_direct_mechanism_checks_passed": True,
            "all_physical_pair_parity_checks_passed": True,
        },
    }

    dev_image_id = "d" * 64
    loaded_run_binding = _artifact(artifacts / "loaded-run-manifest.json")
    loaded_raw_binding = _artifact(artifacts / "loaded-raw-manifest.json")
    loaded_inspect_binding = _artifact(artifacts / "loaded-container-inspect.json")
    loaded_log_binding = _artifact(artifacts / "loaded-container.log")
    loaded_binary_binding = _artifact(artifacts / "loaded-test-execution-kernel")
    loaded_gate_binding = _artifact(artifacts / "loaded-stability-gate.sh")
    loaded_verifier_path = (
        manyforge / _GENERATE.PRODUCERS["loaded_kernel"][0]
    ).resolve()
    loaded_verifier_binding = {
        "path": str(loaded_verifier_path),
        "sha256": _sha256(loaded_verifier_path),
    }
    loaded_ledger_binding = _artifact(artifacts / "loaded-ledger.tsv")
    loaded_inventory_binding = _artifact(artifacts / "loaded-test-inventory.txt")
    loaded_iteration_logs = [
        {
            "iteration": index,
            **_artifact(artifacts / f"loaded-iteration-{index:02d}.log"),
        }
        for index in range(1, 31)
    ]
    loaded_retained = [
        {"role": "loaded_run_manifest", **loaded_run_binding},
        {"role": "loaded_raw_manifest", **loaded_raw_binding},
        {"role": "loaded_container_inspect", **loaded_inspect_binding},
        {"role": "loaded_container_log", **loaded_log_binding},
        {"role": "loaded_test_binary", **loaded_binary_binding},
        {"role": "loaded_gate_script", **loaded_gate_binding},
        {"role": "loaded_verifier_script", **loaded_verifier_binding},
        {"role": "loaded_ledger", **loaded_ledger_binding},
        {"role": "loaded_test_inventory", **loaded_inventory_binding},
        *(
            {"role": "loaded_iteration_log", **binding}
            for binding in loaded_iteration_logs
        ),
    ]
    loaded = {
        "schema_version": _VERIFY.LOADED_SCHEMA,
        "loaded_kernel_gate_passed": True,
        "failure_count": 0,
        "step18_closed": False,
        "source_identity": source,
        "specs_source_identity_at_run": campaign_specs,
        "specs_contract_identity": contract,
        "configuration": {
            "iterations": 30,
            "load_workers": 3,
            "timeout_seconds": 30,
            "tests_per_iteration": 291,
        },
        "iteration_logs": loaded_iteration_logs,
        "container": {"image_id": dev_image_id},
        "run_manifest": loaded_run_binding,
        "raw_manifest": loaded_raw_binding,
        "container_inspect": loaded_inspect_binding,
        "container_log": loaded_log_binding,
        "test_binary": loaded_binary_binding,
        "gate_script": loaded_gate_binding,
        "verifier_script": loaded_verifier_binding,
        "ledger": loaded_ledger_binding,
        "test_inventory": {
            **loaded_inventory_binding,
            "required_predicates": sorted(
                {
                    "ExecutionKernelTest.Cancel_StopDuringValidationRequestsValidationCancel",
                    "ExecutionKernelTest.Cancel_ResetDuringValidationRequestsValidationCancel",
                    "ExecutionKernelTest.Stop_OperatorProfileRefused_EmergencyLegacyFallback",
                    "ExecutionKernelTest.Stop_OperatorDeadlineWatchdogHardStopsBeforeBackendReturns",
                }
            ),
        },
        "retained_artifact_manifest": loaded_retained,
    }

    release_artifact_binding = _artifact(artifacts / "release-campaign.json")
    release_verifier_path = (
        manyforge / _GENERATE.PRODUCERS["release_gates"][0]
    ).resolve()
    release_verifier_binding = {
        "path": str(release_verifier_path),
        "sha256": _sha256(release_verifier_path),
    }
    release_runner_path = (
        manyforge / "scripts/qualification/run_step18_release_gates.py"
    ).resolve()
    release_runner_binding = {
        "path": str(release_runner_path),
        "sha256": _sha256(release_runner_path),
    }
    release_lanes: list[dict[str, Any]] = []
    release_retained: list[dict[str, Any]] = [
        {"role": "release_artifact_manifest", **release_artifact_binding},
        {"role": "release_verifier", **release_verifier_binding},
        {"role": "release_runner", **release_runner_binding},
    ]
    sanitizer_lanes = {
        "core_asan_ubsan",
        "core_tsan",
        "collision_asan_ubsan",
        "collision_tsan",
    }
    for name in sorted(_VERIFY.RELEASE_LANES):
        lane_manifest = _artifact(artifacts / f"release-{name}-manifest.json")
        container_inspect = _artifact(artifacts / f"release-{name}-inspect.json")
        container_log = _artifact(artifacts / f"release-{name}.log")
        lane: dict[str, Any] = {
            "name": name,
            "passed": True,
            "container": {"image_id": dev_image_id},
            "lane_manifest": lane_manifest,
            "container_inspect": container_inspect,
            "container_log": {**container_log, "bytes": 9},
            "security_override": None,
        }
        release_retained.extend(
            (
                {"role": "release_lane_manifest", "lane": name, **lane_manifest},
                {
                    "role": "release_container_inspect",
                    "lane": name,
                    **container_inspect,
                },
                {
                    "role": "release_container_log",
                    "lane": name,
                    **container_log,
                    "bytes": 9,
                },
            )
        )
        if name in sanitizer_lanes:
            security_override = _artifact(
                artifacts / f"release-{name}-security.override.yaml"
            )
            lane["security_override"] = {**security_override, "bytes": 9}
            release_retained.append(
                {
                    "role": "release_security_override",
                    "lane": name,
                    **security_override,
                    "bytes": 9,
                }
            )
        release_lanes.append(lane)
    release = {
        "schema_version": _VERIFY.RELEASE_SCHEMA,
        "verification_scope": "corrected_source_release_gates",
        "release_gates_passed": True,
        "failure_count": 0,
        "overall_step18_closure_claimed": False,
        "requires_common_candidate_aggregator": True,
        "candidate_source_identity": source,
        "candidate_specs_source_identity": campaign_specs,
        "candidate_specs_contract_identity": contract,
        "lanes": release_lanes,
        "artifact_manifest": release_artifact_binding,
        "retained_artifact_manifest": release_retained,
        "verifier": release_verifier_binding,
        "runner": release_runner_binding,
    }

    summary_paths = {
        "offline_proof": evidence_root / "proof-summary.json",
        "scheduled_runtime": evidence_root / "runtime-summary.json",
        "live_contact_modes": evidence_root / "live-summary.json",
        "loaded_kernel": evidence_root / "loaded-summary.json",
        "release_gates": evidence_root / "release-summary.json",
    }
    summaries = {
        "offline_proof": proof,
        "scheduled_runtime": runtime,
        "live_contact_modes": live,
        "loaded_kernel": loaded,
        "release_gates": release,
    }
    for name, path in summary_paths.items():
        _json_write(path, summaries[name])

    evidence_record = specs / _SOURCE.SPECS_CONTRACT_EXCLUDED_PATH
    _write(
        evidence_record,
        _evidence_record(
            summary_paths,
            _QUALIFICATION / "verify_step18_closure.py",
        ),
    )
    arguments = argparse.Namespace(
        manyforge_root=manyforge,
        specs_root=specs,
        offline_proof=summary_paths["offline_proof"],
        scheduled_runtime=summary_paths["scheduled_runtime"],
        live_contact_modes=summary_paths["live_contact_modes"],
        loaded_kernel=summary_paths["loaded_kernel"],
        release_gates=summary_paths["release_gates"],
        evidence_record=evidence_record,
        output=evidence_root / "closure-manifest-unused.json",
    )
    closure_path = evidence_root / "closure-manifest.json"
    _json_write(closure_path, _GENERATE.generate(arguments))

    snapshots = copy.deepcopy(summaries)

    def replay_frozen(
        name: str,
        invocation: dict[str, Any],
        expected_summary: dict[str, Any],
        manyforge_root: Path,
    ) -> dict[str, Any]:
        del manyforge_root
        if _VERIFY._canonical_producer_result(  # noqa: SLF001
            name, expected_summary
        ) != _VERIFY._canonical_producer_result(name, snapshots[name]):  # noqa: SLF001
            raise _VERIFY.ValidationError(
                f"{name} summary differs from a replay of its current bound producer"
            )
        return {
            "verifier": invocation["verifier"],
            "arguments": invocation["arguments"],
            "result_transport": invocation["result_transport"],
            "result_sha256": hashlib.sha256(
                json.dumps(expected_summary, sort_keys=True).encode("utf-8")
            ).hexdigest(),
            "canonical_match": True,
        }

    monkeypatch.setattr(_VERIFY, "_run_producer", replay_frozen)
    return closure_path, {
        "manyforge": manyforge,
        "specs": specs,
        "source": source,
        "campaign_specs": campaign_specs,
        "contract": contract,
        "summary_paths": summary_paths,
        "summaries": summaries,
        "snapshots": snapshots,
        "proof_artifact": Path(proof_manifest[0]["path"]),
        "evidence_record": evidence_record,
        "arguments": arguments,
        "evidence_root": evidence_root,
    }


def _refresh_closure_bindings(closure_path: Path, paths: dict[str, Any]) -> None:
    summary_paths = paths["summary_paths"]
    _write(
        paths["evidence_record"],
        _evidence_record(
            summary_paths,
            _QUALIFICATION / "verify_step18_closure.py",
        ),
    )
    closure = _json_read(closure_path)
    for name, summary_path in summary_paths.items():
        closure["inputs"][name]["sha256"] = _sha256(summary_path)
    closure["evidence_record"]["sha256"] = _sha256(paths["evidence_record"])
    closure["closure_specs_source_identity"] = _SOURCE.source_identity(paths["specs"])
    _json_write(closure_path, closure)


def _main_arguments(paths: dict[str, Any], output: Path) -> list[str]:
    args = paths["arguments"]
    return [
        "--manyforge-root",
        str(args.manyforge_root),
        "--specs-root",
        str(args.specs_root),
        "--offline-proof",
        str(args.offline_proof),
        "--scheduled-runtime",
        str(args.scheduled_runtime),
        "--live-contact-modes",
        str(args.live_contact_modes),
        "--loaded-kernel",
        str(args.loaded_kernel),
        "--release-gates",
        str(args.release_gates),
        "--evidence-record",
        str(args.evidence_record),
        "--output",
        str(output),
    ]


def test_step18_closure_accepts_only_complete_common_candidate(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, _ = _fixture(tmp_path, monkeypatch)
    result = _VERIFY.verify(manifest)
    assert result["step18_closed"] is True
    assert result["overall_step18_closure_claimed"] is True
    assert result["subgate_count"] == 5
    assert set(result["producer_replays"]) == set(_GENERATE.INPUTS)
    assert result["loaded_release_image_parity"]["passed"] is True
    assert result["live_mechanism_ownership"]["passed"] is True
    assert result["production_hygiene"]["passed"] is True
    assert result["production_hygiene"]["distance_capability_contract"] == {
        "descriptor_field_present": True,
        "declared_modes": [
            "kNone",
            "kSelectedCollidingPairs",
            "kAllCollidingPairs",
            "kAllActivePairs",
        ],
        "declaration_sorted_unique": True,
        "exact_scope_lookup": True,
        "validation_exact_admission_paths": 2,
        "runtime_exact_admission_paths": 1,
        "mode_scoped_capability_requirements": True,
        "broader_fallback_present": False,
        "passed": True,
    }


@pytest.mark.parametrize(
    "mutation",
    (
        pytest.param(
            lambda proof: proof["precise_reference_parity"].__setitem__(
                "timing_gate_excluded", False
            ),
            id="timing-gate-inclusion",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"].__setitem__(
                "record_count", 5
            ),
            id="raw-record-count",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"].__setitem__(
                "runs_per_source_role", 2
            ),
            id="source-repetitions",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"]["case_order"].reverse(),
            id="case-order",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"].__setitem__(
                "semantic_parity", False
            ),
            id="semantic-parity",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"].__setitem__(
                "current_transcript_sha256", "d" * 64
            ),
            id="normalized-transcript-mismatch",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"].__setitem__(
                "unexpected", True
            ),
            id="unexpected-parity-field",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "cross_worker_orchestration_projection"
            ].__setitem__("excluded_result_fields", []),
            id="query-id-exclusion-missing",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "cross_worker_orchestration_projection"
            ].__setitem__(
                "excluded_result_fields", ["query_id", "engine_status_code"]
            ),
            id="orchestration-exclusion-widened",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "cross_worker_orchestration_projection"
            ]["small_case_profile_sha256"].__setitem__("5", "d" * 64),
            id="cross-worker-projection-drift",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ].pop(),
            id="per-record-transcript-missing",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ][1].__setitem__("repetition", 1),
            id="repetition-duplicate",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ][0].__setitem__("artifact_sha256", "d" * 64),
            id="raw-artifact-digest-unbound",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ][0].__setitem__("run_id", "unbound-run"),
            id="raw-artifact-identity-unbound",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ][0].__setitem__("input_corpus_sha256", "d" * 64),
            id="per-record-input-corpus-drift",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ][0]["small_case_profile_sha256"].pop("5"),
            id="per-record-small-profile-missing",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ][0]["small_case_sha256"]["3"].pop(
                _VERIFY.PRECISE_REFERENCE_CASE_ORDER[-1]
            ),
            id="per-case-digest-missing",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ][0]["partition_profile_sha256"].__setitem__("4", "d" * 64),
            id="per-record-partition-drift",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ][0]["completion_order_sha256"].__setitem__("serial", "d" * 64),
            id="per-record-completion-drift",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "per_record_transcripts"
            ][0]["exceptional_case_sha256"].__setitem__(
                "worker_exception", "d" * 64
            ),
            id="per-record-exceptional-drift",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "partition_inventory"
            ].__setitem__("worker_counts", [2, 3, 4]),
            id="partition-worker-profile",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "partition_inventory"
            ].__setitem__("patterns_per_worker", 1023),
            id="partition-pattern-count",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "partition_inventory"
            ].__setitem__("total_partition_evaluations_per_record", 4095),
            id="partition-total",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "partition_inventory"
            ].__setitem__("parity", False),
            id="partition-parity",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "partition_inventory"
            ]["matched_profile_sha256"].__setitem__("2", "d" * 64),
            id="partition-summary-digest-drift",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "completion_order_inventory"
            ]["orders"].pop(),
            id="completion-order-missing",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "completion_order_inventory"
            ].__setitem__("workers", 4),
            id="completion-worker-profile",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "completion_order_inventory"
            ]["matched_profile_sha256"].__setitem__("serial", "d" * 64),
            id="completion-summary-digest-drift",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "exceptional_cases"
            ].pop("worker_exception"),
            id="exceptional-profile-missing",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "exceptional_cases"
            ]["expired_deadline"].__setitem__("parity", False),
            id="exceptional-parity",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "exceptional_cases"
            ]["worker_exception"].__setitem__("status_code", 4),
            id="exceptional-status-code",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "normalization_audit"
            ]["pristine_baseline_timing"].__setitem__(
                "baseline_legacy_zero_observations", 0
            ),
            id="baseline-legacy-zero-proof-missing",
        ),
        pytest.param(
            lambda proof: proof["precise_reference_parity"][
                "normalization_audit"
            ]["current_candidate"].__setitem__(
                "current_raw_mode_present_profiles", 49
            ),
            id="current-mode-presence-audit",
        ),
        pytest.param(
            lambda proof: proof["performance"].__setitem__(
                "precise_reference_parity", {"passed": True}
            ),
            id="timing-performance-contamination",
        ),
        pytest.param(
            lambda proof: next(
                item
                for item in proof["artifact_manifest"]
                if item.get("cell") == "precise_reference_parity"
            ).__setitem__("cell", "policy_support_contact"),
            id="raw-artifact-missing",
        ),
        pytest.param(
            lambda proof: next(
                item
                for item in proof["artifact_manifest"]
                if item.get("cell") == "precise_reference_parity"
            ).__setitem__("arm", "signed_penetration_reference"),
            id="raw-artifact-fabricated-profile",
        ),
    ),
)
def test_step18_closure_rejects_incomplete_precise_reference_parity(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    mutation: Any,
) -> None:
    _, paths = _fixture(tmp_path, monkeypatch)
    proof = copy.deepcopy(paths["summaries"]["offline_proof"])
    mutation(proof)
    with pytest.raises(_VERIFY.ValidationError, match="precise-reference"):
        _VERIFY._verify_proof(  # noqa: SLF001
            proof,
            paths["source"],
            paths["campaign_specs"],
            paths["contract"],
        )


def test_step18_closure_accepts_only_evidence_record_specs_drift(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    _write(
        paths["evidence_record"],
        _evidence_record(
            paths["summary_paths"],
            _QUALIFICATION / "verify_step18_closure.py",
            prose="updated evidence prose\n",
        ),
    )
    closure = _json_read(manifest)
    closure["evidence_record"]["sha256"] = _sha256(paths["evidence_record"])
    closure["closure_specs_source_identity"] = _SOURCE.source_identity(paths["specs"])
    _json_write(manifest, closure)
    result = _VERIFY.verify(manifest)
    assert result["campaign_specs_source_identity"] == paths["campaign_specs"]
    assert result["closure_specs_source_identity"] != paths["campaign_specs"]
    assert result["candidate_specs_contract_identity"] == paths["contract"]


@pytest.mark.parametrize("name", _GENERATE.INPUTS)
def test_step18_closure_rejects_rehashed_forged_producer_summary(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, name: str
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    summary_path = paths["summary_paths"][name]
    summary = _json_read(summary_path)
    summary["forged_unverified_claim"] = True
    _json_write(summary_path, summary)
    _refresh_closure_bindings(manifest, paths)
    with pytest.raises(_VERIFY.ValidationError, match="differs from a replay"):
        _VERIFY.verify(manifest)


def test_step18_closure_rejects_retained_artifact_substitution(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    _write(paths["proof_artifact"], "substituted proof payload\n")
    with pytest.raises(_VERIFY.ValidationError, match="changed"):
        _VERIFY.verify(manifest)


def test_step18_closure_rejects_mixed_campaign_specs_identity(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _, paths = _fixture(tmp_path, monkeypatch)
    other = tmp_path / "other-specs"
    _commit_repo(other, {"contract.md": "other\n"})
    _write(other / "delta.md", "other delta\n")
    loaded_path = paths["summary_paths"]["loaded_kernel"]
    loaded = _json_read(loaded_path)
    loaded["specs_source_identity_at_run"] = _SOURCE.source_identity(other)
    _json_write(loaded_path, loaded)
    with pytest.raises(_GENERATE.ManifestError, match="share one full Specs"):
        _GENERATE.generate(paths["arguments"])


def test_step18_closure_rejects_loaded_release_dev_image_mismatch(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    loaded_path = paths["summary_paths"]["loaded_kernel"]
    loaded = _json_read(loaded_path)
    loaded["container"]["image_id"] = "e" * 64
    _json_write(loaded_path, loaded)
    paths["snapshots"]["loaded_kernel"] = copy.deepcopy(loaded)
    _refresh_closure_bindings(manifest, paths)
    with pytest.raises(_VERIFY.ValidationError, match="different dev images"):
        _VERIFY.verify(manifest)


def test_step18_closure_rejects_wrong_evidence_subgate_hash(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    claim = _GENERATE.parse_evidence_record(paths["evidence_record"].read_bytes())
    claim["subgates"]["loaded_kernel"]["summary_sha256"] = "0" * 64
    _write(
        paths["evidence_record"],
        _GENERATE.EVIDENCE_BEGIN
        + "\n"
        + json.dumps(claim, sort_keys=True)
        + "\n"
        + _GENERATE.EVIDENCE_END
        + "\n",
    )
    closure = _json_read(manifest)
    closure["evidence_record"]["sha256"] = _sha256(paths["evidence_record"])
    closure["closure_specs_source_identity"] = _SOURCE.source_identity(paths["specs"])
    _json_write(manifest, closure)
    with pytest.raises(_VERIFY.ValidationError, match="does not bind the exact loaded"):
        _VERIFY.verify(manifest)


def test_step18_closure_rejects_wrong_evidence_verifier_hash(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    claim = _GENERATE.parse_evidence_record(paths["evidence_record"].read_bytes())
    claim["closure_verifier"]["sha256"] = "0" * 64
    _write(
        paths["evidence_record"],
        _GENERATE.EVIDENCE_BEGIN
        + "\n"
        + json.dumps(claim, sort_keys=True)
        + "\n"
        + _GENERATE.EVIDENCE_END
        + "\n",
    )
    closure = _json_read(manifest)
    closure["evidence_record"]["sha256"] = _sha256(paths["evidence_record"])
    closure["closure_specs_source_identity"] = _SOURCE.source_identity(paths["specs"])
    _json_write(manifest, closure)
    with pytest.raises(
        _VERIFY.ValidationError, match="does not bind the exact closure verifier"
    ):
        _VERIFY.verify(manifest)


def test_step18_closure_rejects_noncanonical_evidence_record_path(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    impostor = tmp_path / "evidence-copy.md"
    _write(impostor, paths["evidence_record"].read_text(encoding="utf-8"))
    closure = _json_read(manifest)
    closure["evidence_record"] = {
        "path": str(impostor.resolve()),
        "sha256": _sha256(impostor),
        "schema_version": _GENERATE.EVIDENCE_SCHEMA,
    }
    _json_write(manifest, closure)
    with pytest.raises(_VERIFY.ValidationError, match="singleton excluded path"):
        _VERIFY.verify(manifest)


def test_step18_closure_rejects_mutated_producer_invocation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, _ = _fixture(tmp_path, monkeypatch)
    closure = _json_read(manifest)
    closure["producer_invocations"]["loaded_kernel"]["arguments"].append("--forged")
    _json_write(manifest, closure)
    with pytest.raises(_VERIFY.ValidationError, match="typed summary inputs"):
        _VERIFY.verify(manifest)


def test_step18_closure_requires_same_fixture_live_mechanism_owner(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    live_path = paths["summary_paths"]["live_contact_modes"]
    live = _json_read(live_path)
    live["live_mechanism_replay"]["owner_scope"] = "synthetic_ur5_delegation"
    _json_write(live_path, live)
    paths["snapshots"]["live_contact_modes"] = copy.deepcopy(live)
    _refresh_closure_bindings(manifest, paths)
    with pytest.raises(_VERIFY.ValidationError, match="UR10/Isaac mechanism ownership"):
        _VERIFY.verify(manifest)


def test_step18_closure_detects_retained_input_toctou(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    original = _VERIFY._production_hygiene  # noqa: SLF001

    def mutate_after_reads(repo_root: Path) -> dict[str, Any]:
        result = original(repo_root)
        _write(paths["proof_artifact"], "changed after initial verification\n")
        return result

    monkeypatch.setattr(_VERIFY, "_production_hygiene", mutate_after_reads)
    with pytest.raises(_VERIFY.ValidationError, match="changed"):
        _VERIFY.verify(manifest)


def test_step18_closure_detects_source_toctou(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    original = _VERIFY._production_hygiene  # noqa: SLF001

    def mutate_source(repo_root: Path) -> dict[str, Any]:
        result = original(repo_root)
        _write(paths["manyforge"] / "late-source-delta.txt", "late\n")
        return result

    monkeypatch.setattr(_VERIFY, "_production_hygiene", mutate_source)
    with pytest.raises(_VERIFY.ValidationError, match="source changed"):
        _VERIFY.verify(manifest)


def test_step18_closure_replay_output_stays_in_ignored_evidence_root(
    tmp_path: Path,
) -> None:
    manyforge = tmp_path / "manyforge"
    _commit_repo(manyforge, {".gitignore": ".manyforge-var/\n"})
    verifier = tmp_path / "producer.py"
    record = tmp_path / "output-path.txt"
    _write(
        verifier,
        """#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
parser = argparse.ArgumentParser()
parser.add_argument('--record', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.record.write_text(str(args.output.resolve()), encoding='utf-8')
args.output.write_text(json.dumps({'passed': True}) + '\\n', encoding='utf-8')
""",
    )
    invocation = {
        "verifier": {"path": str(verifier), "sha256": _sha256(verifier)},
        "arguments": ["--record", str(record)],
        "result_transport": "output_json",
    }
    result = _VERIFY._run_producer(  # noqa: SLF001
        "live_contact_modes", invocation, {"passed": True}, manyforge
    )
    output_path = Path(record.read_text(encoding="utf-8"))
    output_path.relative_to(manyforge / ".manyforge-var/qualification")
    assert result["canonical_match"] is True
    assert not output_path.exists()


def test_step18_manifest_generation_removes_failed_postwrite_output(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _, paths = _fixture(tmp_path, monkeypatch)
    output = paths["evidence_root"] / "postwrite-manifest.json"
    calls = 0

    def changing_generate(args: Any) -> dict[str, int]:
        nonlocal calls
        calls += 1
        if calls == 2:
            assert not output.exists()
            assert list(output.parent.glob(f".{output.name}.tmp-*"))
        return {"version": calls}

    monkeypatch.setattr(_GENERATE, "generate", changing_generate)
    assert _GENERATE.main(_main_arguments(paths, output)) == 2
    assert not output.exists()
    assert not list(output.parent.glob(f".{output.name}.tmp-*"))


def test_step18_verifier_refuses_unignored_output(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    monkeypatch.setattr(
        _VERIFY,
        "verify",
        lambda ignored: {"manyforge_root": str(paths["manyforge"])},
    )
    output = tmp_path / "published-closure.json"
    assert _VERIFY.main(["--manifest", str(manifest), "--output", str(output)]) == 2
    assert not output.exists()


def test_step18_verifier_removes_output_when_postwrite_binding_changes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest, paths = _fixture(tmp_path, monkeypatch)
    result = _VERIFY.verify(manifest)
    monkeypatch.setattr(_VERIFY, "verify", lambda ignored: result)
    original = _VERIFY._assert_result_bindings_current  # noqa: SLF001

    def mutate_binding(value: Any) -> None:
        _write(paths["proof_artifact"], "changed before publication\n")
        original(value)

    monkeypatch.setattr(_VERIFY, "_assert_result_bindings_current", mutate_binding)
    output = paths["evidence_root"] / "closure-result.json"
    assert _VERIFY.main(["--manifest", str(manifest), "--output", str(output)]) == 2
    assert not output.exists()
    assert not list(output.parent.glob(f".{output.name}.tmp-*"))


def test_step18_closure_hygiene_rejects_obsolete_production_field(
    tmp_path: Path,
) -> None:
    repo = tmp_path / "source"
    _write(
        repo / "manyforge_core/src/policy.cpp",
        'auto field = "min_separation_clearance_m";\n',
    )
    _write(
        repo / "manyforge_collision/src/manyforge_collision_engine.cpp",
        "if (x == kAllActivePairs) computeDistances(a);\n"
        "if (y == kAllActivePairs) computeDistances(b);\n"
        "computeDistance(a); computeDistance(b);\n",
    )
    with pytest.raises(_VERIFY.ValidationError, match="obsolete separation-clearance"):
        _VERIFY._production_hygiene(repo)  # noqa: SLF001


@pytest.mark.parametrize(
    ("relative", "old", "new", "message"),
    (
        (
            "manyforge_core/include/manyforge_core/types/collision.hpp",
            "supported_distance_request_modes;",
            "legacy_distance_request_modes;",
            "descriptor",
        ),
        (
            "manyforge_core/include/manyforge_core/types/collision.hpp",
            "return std::binary_search(",
            "return std::find(",
            "exact declared mode",
        ),
        (
            "manyforge_collision/src/manyforge_collision_engine.cpp",
            "      core::CollisionDistanceRequestMode::kSelectedCollidingPairs,\n",
            "",
            "exact sorted four-mode",
        ),
        (
            "manyforge_core/src/collision_validation_coordinator.cpp",
            "collision_engine_supports_distance_request_mode(\n          capability, distance_request_mode)",
            "capability.supports_distance_query",
            "validation",
        ),
        (
            "manyforge_core/src/runtime_collision_query_session.cpp",
            "collision_engine_supports_distance_request_mode(*capability_,",
            "legacy_broad_distance_admission(*capability_,",
            "runtime",
        ),
    ),
)
def test_step18_closure_hygiene_rejects_distance_capability_regression(
    tmp_path: Path, relative: str, old: str, new: str, message: str
) -> None:
    repo = _copy_distance_capability_sources(tmp_path / "source")
    path = repo / relative
    text = path.read_text(encoding="utf-8")
    assert old in text
    _write(path, text.replace(old, new, 1))
    with pytest.raises(_VERIFY.ValidationError, match=message):
        _VERIFY._distance_capability_contract_hygiene(repo)  # noqa: SLF001
