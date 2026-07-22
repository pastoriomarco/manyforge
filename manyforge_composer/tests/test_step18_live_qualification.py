from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import shlex
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any

import pytest

from scripts.qualification import roboplan_05_qualification as _QUAL
from scripts.qualification import generate_step18_live_contact_modes_manifest as _GEN
from scripts.qualification import run_step18_live_contact_mode as _RUN
from scripts.qualification import source_identity as _SOURCE_ID


_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "scripts/qualification/verify_step18_live_contact_modes.py"
_RUNNER = _REPO_ROOT / "scripts/qualification/run_step18_live_contact_mode.py"
_SPEC = importlib.util.spec_from_file_location(
    "verify_step18_live_contact_modes", _SCRIPT
)
assert _SPEC is not None and _SPEC.loader is not None
_VERIFY = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_VERIFY)


_FROZEN_CANDIDATE_DELTA_SHA256 = _VERIFY._delta_sha256(  # noqa: SLF001
    b"step18-live-unit-test-candidate"
)


def _frozen_candidate_source_identity(root: Path) -> dict[str, Any]:
    """Model the qualified pre-commit candidate after Step 18 closure.

    The production verifier intentionally accepts only the dirty candidate
    rooted at 2bfe30e.  Once that candidate was committed, the repository's
    real HEAD could no longer serve as the unit-test fixture.  Keep the
    verifier strict and supply its frozen source identity at the test seam.
    """

    if Path(root).resolve() != _REPO_ROOT.resolve():
        return _SOURCE_ID.source_identity(root)
    return _VERIFY._source_identity(  # noqa: SLF001
        head_commit=_VERIFY.PINNED_BASELINE_COMMIT,
        head_tree=_VERIFY.PINNED_BASELINE_TREE,
        dirty=True,
        delta_sha256=_FROZEN_CANDIDATE_DELTA_SHA256,
    )


@pytest.fixture(autouse=True)
def _use_frozen_candidate_source_identity(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        _VERIFY, "current_source_identity", _frozen_candidate_source_identity
    )
    monkeypatch.setattr(
        _GEN.live, "current_source_identity", _frozen_candidate_source_identity
    )
    monkeypatch.setattr(_GEN, "source_identity", _frozen_candidate_source_identity)


def _specs_repository(tmp_path: Path) -> Path:
    """Create the real, dirty Specs checkout bound by one live-test campaign.

    The production verifier pins the real baseline commit and tree.  These unit
    tests need the same cross-repository checks without assuming that a sibling
    ManyForge Specs checkout is mounted in the standard development container.
    Build a minimal Git baseline, then change one tracked normative file to
    model the candidate contract delta.  Both independently imported verifier
    modules use that real commit/tree as their test-local pinned baseline.
    """

    repo = tmp_path / "manyforge_specs"
    if not repo.exists():
        repo.mkdir()
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(
            ["git", "-C", str(repo), "config", "user.email", "test@example.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(repo), "config", "user.name", "Step18 Specs Test"],
            check=True,
        )
        contract = repo / "docs/spec/250-contact-policy.md"
        contract.parent.mkdir(parents=True)
        contract.write_text("baseline contact-policy contract\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
        subprocess.run(
            ["git", "-C", str(repo), "commit", "-q", "-m", "baseline"],
            check=True,
        )
        contract.write_text(
            "baseline contact-policy contract\nStep 18 candidate contract\n",
            encoding="utf-8",
        )

    head_commit = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True
    ).strip()
    head_tree = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD^{tree}"], text=True
    ).strip()
    _VERIFY.PINNED_SPECS_COMMIT = head_commit
    _VERIFY.PINNED_SPECS_TREE = head_tree
    _GEN.live.PINNED_SPECS_COMMIT = head_commit
    _GEN.live.PINNED_SPECS_TREE = head_tree
    return repo


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _canonical_sha256(value: Any) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def _declared_path_sha_bindings(value: Any) -> list[dict[str, Any]]:
    bindings: list[dict[str, Any]] = []

    def visit(node: Any) -> None:
        if isinstance(node, dict):
            if "path" in node and "sha256" in node:
                bindings.append(node)
            for child in node.values():
                visit(child)
        elif isinstance(node, list):
            for child in node:
                visit(child)

    visit(value)
    return bindings


def _runtime(name: str, host_root: str, marker: str) -> dict[str, Any]:
    artifacts = [
        {
            "path": f"/tmp/manyforge-build/{marker}/libmanyforge_{marker}.so",
            "sha256": hashlib.sha256(f"native-{marker}".encode()).hexdigest(),
        }
    ]
    process_payload = {
        "processes": [
            {
                "pid": 1,
                "exe": "/usr/bin/python3",
                "exe_sha256": "1" * 64,
                "cmdline": f"python manyforge-{marker}",
                "mapped_native_artifacts": artifacts,
            }
        ],
        "mapped_native_artifacts": artifacts,
    }
    process = {
        **process_payload,
        "fingerprint_sha256": _canonical_sha256(process_payload),
    }
    payload = {
        "name": name,
        "id": hashlib.sha256(f"container-{marker}".encode()).hexdigest(),
        "image_id": "sha256:" + "a" * 64,
        "image_name": "manyforge-ros-jazzy-full",
        "started_at": "2026-07-20T00:00:00.000000Z",
        "log_config": {"Type": "json-file", "Config": {}},
        "workspace_mount": {
            "type": "bind",
            "source": host_root,
            "destination": "/workspace",
            "rw": True,
        },
        "process": process,
    }
    return {**payload, "fingerprint_sha256": _canonical_sha256(payload)}


def _profile(composer: dict[str, Any], controller: dict[str, Any], host_root: str) -> dict[str, Any]:
    return {
        "host_repo_root": host_root,
        "composer_runtime_fingerprint_sha256": composer["fingerprint_sha256"],
        "controller_runtime_fingerprint_sha256": controller["fingerprint_sha256"],
        "composer_image_id": composer["image_id"],
        "controller_image_id": controller["image_id"],
        "composer_native_artifacts_sha256": _canonical_sha256(
            composer["process"]["mapped_native_artifacts"]
        ),
        "controller_native_artifacts_sha256": _canonical_sha256(
            controller["process"]["mapped_native_artifacts"]
        ),
    }


def _authority(mode: str) -> dict[str, Any]:
    return {
        "revision": {"revision_id": 7, "content_hash": "scene-seven"},
        "transaction_sequence": 11,
        "scene_admission_epoch": 3,
        "topology_epoch": 5,
        "policy_epoch": 2,
        "configured_collision_policy": {
            "policy_id": "default_free_space",
            "revision": {"revision_id": 9, "content_hash": "policy-nine"},
            "support_contacts": [{"mode": mode}],
        },
    }


def _specs_contract(*, dirty: bool, delta_sha256: str) -> dict[str, Any]:
    return _VERIFY._specs_contract_identity(  # noqa: SLF001
        head_commit=_VERIFY.PINNED_SPECS_COMMIT,
        head_tree=_VERIFY.PINNED_SPECS_TREE,
        contract_dirty=dirty,
        contract_delta_sha256=delta_sha256,
    )


def _source(identity: dict[str, Any]) -> dict[str, Any]:
    frozen = identity["dirty"]
    return {
        "manyforge_dirty": frozen,
        "manyforge_commit": identity["head_commit"],
        "manyforge_worktree_sha256": identity["delta_sha256"],
        "manyforge_worktree_delta_sha256": identity["delta_sha256"],
        "manyforge_source_fingerprint_sha256": identity["fingerprint_sha256"],
        "manyforge_source_identity": identity,
        "manyforge_source_identity_mode": (
            "frozen_source_fingerprint_sha256" if frozen else "clean_git_commit"
        ),
        "manyforge_expected_source_fingerprint_sha256": identity[
            "fingerprint_sha256"
        ],
        "roboplan_commit": _QUAL.ROBOPLAN_05_PINNED_COMMIT,
        "roboplan_ref": _QUAL.ROBOPLAN_05_PINNED_COMMIT,
        "roboplan_version": _QUAL.ROBOPLAN_05_VERSION,
        "roboplan_source_clean": True,
        "roboplan_package_versions": copy.deepcopy(_QUAL.ROBOPLAN_05_PACKAGE_VERSIONS),
        "roboplan_patch_manifest_sha256": _QUAL.ROBOPLAN_05_PATCH_MANIFEST_SHA256,
        "roboplan_patches": [
            {"path": path, "sha256": digest}
            for path, digest in sorted(_QUAL.ROBOPLAN_05_PATCH_DIGESTS.items())
        ],
        "pinocchio_version": _QUAL.ROBOPLAN_05_PINOCCHIO_VERSION,
        "coal_version": _QUAL.ROBOPLAN_05_COAL_VERSION,
        "eigenpy_version": "3.12.0",
        "program": {
            "name": "isaac_manipulator_scene_light",
            "path": "examples/isaac_manipulator_scene_light.program.yaml",
            "sha256": "1" * 64,
            "loaded_payload_sha256": "2" * 64,
        },
        "deployment": {
            "name": "isaac_manipulator_scene_light_ros2_control",
            "path": "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml",
            "sha256": "3" * 64,
            "loaded_payload_sha256": "4" * 64,
        },
        "artifact_reload": {
            "schema_version": "manyforge.step18_artifact_reload.v1",
            "endpoint": "/api/program/load",
            "program_path": (
                "examples/isaac_manipulator_scene_light.program.yaml"
            ),
            "deployment_path": (
                "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml"
            ),
            "force_discard_overrides": True,
            "response_payload_sha256": "2" * 64,
        },
        "matched_input_sha256": "5" * 64,
    }


def _contact(mode: str, authority: dict[str, Any]) -> dict[str, Any]:
    bounded = 0.03 if mode == "bounded_penetration" else None
    authored = {
        "support_object_id": "isaac_table_low_327",
        "mode": "bounded_penetration" if bounded is not None else "contact_release",
    }
    resolved = {
        "pair": {"body_a": "isaac_cheese_box", "body_b": "isaac_table_low_327"},
        "mode": authored["mode"],
    }
    if bounded is not None:
        authored["max_contact_penetration_m"] = bounded
        resolved["max_contact_penetration_m"] = bounded
    elif mode != _VERIFY.BASELINE_MODE:
        authored["release_evidence_mode"] = mode
        resolved["release_evidence_mode"] = mode
    else:
        authored["release_evidence_mode"] = "signed_penetration"
        resolved["release_evidence_mode"] = "signed_penetration"
    return {
        "mode": mode,
        "bounded_penetration_m": bounded,
        "saved_scene_sha256": "6" * 64,
        "effective_scene_sha256": hashlib.sha256(mode.encode()).hexdigest(),
        "authored_contact": authored,
        "resolved_contact": resolved,
        "scene_authority_after_apply": authority,
    }


def _mechanism_capture(mode: str) -> dict[str, Any]:
    capture = json.loads(
        (
            _REPO_ROOT
            / "benchmarks/roboplan_05/corpus/captures/isaac-light-drop-approach-v1.json"
        ).read_text(encoding="utf-8")
    )
    names = capture["start_state"]["joint_names"]
    positions = capture["start_state"]["positions_rad"]
    capture["robot_model_authority"]["group_commandable_joint_names"]["arm"] = names
    groups = [
        {
            "group_id": "arm",
            "joint_names": names,
            "positions_rad": positions,
            "source": "exact_plan_request",
        },
        {
            "group_id": "gripper",
            "joint_names": ["finger_joint"],
            "positions_rad": [0.2],
            "source": "fresh_visual_snapshot_gripper",
        },
    ]
    capture["scene"]["robot"]["collision_projection_seed"] = {
        "groups": groups,
        "sha256": _canonical_sha256(groups),
    }
    trajectory = {
        "joint_names": names,
        "waypoints": [
            {
                "time_from_start_s": 0.1,
                "positions_rad": positions,
                "velocities_rad_s": [0.0] * len(names),
            },
            {
                "time_from_start_s": 0.2,
                "positions_rad": positions,
                "velocities_rad_s": [0.0] * len(names),
            },
        ],
    }
    trajectory_sha256 = _canonical_sha256(trajectory)
    capture["planning_result"] = {
        "scene_revision": copy.deepcopy(capture["scene"]["authority"]["revision"]),
        "trajectory": trajectory,
        "trajectory_sha256": trajectory_sha256,
    }
    capture["source"]["planning_execution_id"] = 1
    pair = {
        "body_a": "object/isaac_cheese_box/shape/0",
        "body_b": "robot/ur10e/link/wrist_3_link",
    }
    if mode == "bounded_penetration":
        authored = {
            "support_object_id": "isaac_table_low_327",
            "mode": "bounded_penetration",
            "max_contact_penetration_m": 0.03,
        }
        resolved = {
            "pair": pair,
            "mode": "bounded_penetration",
            "max_contact_penetration_m": 0.03,
        }
    else:
        evidence_mode = (
            "signed_penetration" if mode == _VERIFY.BASELINE_MODE else mode
        )
        signed = evidence_mode == "signed_penetration"
        authored = {
            "support_object_id": "isaac_table_low_327",
            "mode": "contact_release",
            "release_evidence_mode": evidence_mode,
            "enforce_max_start_penetration": signed,
            "max_start_penetration_m": 0.03 if signed else 0.0,
            "release_tolerance_m": 0.001 if signed else 0.0,
            "min_release_progress_m": 0.0,
            "require_separation_by_end": True,
            "require_strictly_decreasing_penetration": signed,
            "allow_recontact": False,
        }
        resolved = {"pair": pair, **authored}
        resolved.pop("support_object_id")
    cheese = next(
        item
        for item in capture["scene"]["objects"]
        if item["object_id"] == "isaac_cheese_box"
    )
    cheese["support_contacts"] = [authored]
    capture["scene"]["authority"]["configured_collision_policy"][
        "support_contacts"
    ] = [resolved]
    capture.pop("semantic_sha256", None)
    capture["semantic_sha256"] = _canonical_sha256(capture)
    return capture


def _policy_contact(arm: str) -> dict[str, Any]:
    value = {
        "pair": {
            "body_a": "object/isaac_cheese_box/shape/0",
            "body_b": "robot/ur10e/link/wrist_3_link",
        },
        "mode": 0,
        "release_evidence_mode": 0,
        "enforce_max_start_penetration": True,
        "max_start_penetration_m": 0.03,
        "release_tolerance_m": 0.001,
        "min_release_progress_m": 0.0,
        "max_contact_penetration_m": 0.0,
        "require_separation_by_end": True,
        "require_strictly_decreasing_penetration": True,
        "allow_recontact": False,
    }
    if arm == "binary_contact":
        value.update(
            {
                "release_evidence_mode": 1,
                "enforce_max_start_penetration": False,
                "max_start_penetration_m": 0.0,
                "release_tolerance_m": 0.0,
                "require_strictly_decreasing_penetration": False,
            }
        )
    elif arm == "bounded_penetration":
        value.update(
            {
                "mode": 1,
                "enforce_max_start_penetration": False,
                "max_start_penetration_m": 0.0,
                "release_tolerance_m": 0.0,
                "max_contact_penetration_m": 0.03,
                "require_separation_by_end": False,
                "require_strictly_decreasing_penetration": False,
            }
        )
    return value


def _replay_artifact(
    *,
    capture_identity: dict[str, Any],
    run_id: str,
    arm: str,
    source_role: str,
    observation: bool,
) -> dict[str, Any]:
    pair = {
        "body_a": "object/isaac_cheese_box/shape/0",
        "body_b": "robot/ur10e/link/wrist_3_link",
    }
    transcript = [
        {
            "waypoint_index": index,
            "in_collision": True,
            "engine_status": 0,
            "decision": 0,
            "normalized_pairs": [pair],
        }
        for index in range(2)
    ]
    counters = {
        "query_count": 2,
        "collision_api_calls": 2,
        "active_collision_pairs": 2,
        "colliding_geometry_pairs": 2,
        "colliding_logical_pairs": 2,
        "distance_api_calls": 2,
        "distance_geometry_pairs": 2,
        "distance_logical_pairs": 2,
        "fk_placement_invocations": 2,
        "undeclared_colliding_logical_pairs": 0,
        "distance_mode_mask": 8 if source_role == "baseline_observation" else 2,
        "collision_completion_ordinal_failures": 0,
    }
    if not observation:
        counters = {name: 0 for name in counters}
    elif arm == "binary_contact":
        counters.update(
            {
                "distance_api_calls": 0,
                "distance_geometry_pairs": 0,
                "distance_logical_pairs": 0,
                "distance_mode_mask": 1,
            }
        )
    return {
        "schema_version": _VERIFY.MECHANISM_REPLAY_SCHEMA,
        "run_id": run_id,
        "arm": arm,
        "measurement_kind": "observation" if observation else "decision",
        "source_role": source_role,
        "input": {
            "capture_id": capture_identity["capture_id"],
            "semantic_sha256": capture_identity["capture_semantic_sha256"],
            "trajectory_sha256": capture_identity["trajectory_sha256"],
            "projection_seed_sha256": capture_identity["projection_seed_sha256"],
            "captured_scene_content_hash": capture_identity[
                "captured_scene_content_hash"
            ],
        },
        "replay": {
            "world_content_hash": "world-hash",
            "policy_content_hash": "policy-hash-" + arm,
            "input_waypoint_count": 2,
            "expanded_waypoint_count": 2,
            "expanded_joint_count": 7,
        },
        "policy_contract": {"support_contacts": [_policy_contact(arm)]},
        "decision": {"accepted": True, "action": 0, "first_failing_waypoint": None},
        "mechanism": counters,
        "pair_transcript_fingerprint": "0123456789abcdef",
        "pair_transcript": transcript,
    }
def _progress(*, pristine: bool, p95_ns: float, max_ns: float) -> dict[str, Any]:
    samples = [
        {
            "counter_value": index + 1,
            "latency_ns": int(max_ns if index == 29 else p95_ns),
            "native_max_ns": int(max_ns),
            "observed_monotonic_ns": index + 1,
            "quiescent_reads": 3 if pristine else 0,
        }
        for index in range(30)
    ]
    return {
        "source": (
            "collector_quiescent_repeated_counter_transition"
            if pristine
            else "coherent_native_counter_transition"
        ),
        "instrumentation": (
            "collector_side_unpatched_backend"
            if pristine
            else "native_coherent_observer"
        ),
        "completeness_rule": "exact",
        "baseline_counter": 0,
        "final_counter": 30,
        "native_counter_delta": 30,
        "native_max_ns": int(max_ns),
        "latency_ns": {
            "p50": p95_ns,
            "p95": p95_ns,
            "p99": max_ns,
            "max": max_ns,
        },
        "samples": samples,
    }


def _artifact(
    *,
    identity: dict[str, Any],
    row_uuid: str,
    mode: str,
    stratum: str,
    pristine: bool,
    capture_mechanism: bool,
    p95_ns: float,
    max_ns: float,
) -> dict[str, Any]:
    authority = _authority(mode)
    capture = _mechanism_capture(mode) if capture_mechanism else None
    observer = "quiescent_repeated" if pristine else "coherent_native"
    evidence_command = [
        "/usr/bin/python3",
        "/tmp/tools/scripts/qualification/run_roboplan_05_live_baseline.py",
        "--run-id",
        row_uuid,
        "--viewer-stratum",
        stratum,
        "--measurement-mode",
        "primary",
        "--support-contact-mode",
        mode,
        "--max-contact-penetration-m",
        "0.03",
        "--successful-observations",
        "30",
        "--poll-period-s",
        "0.2",
        "--phase",
        "rp5_7",
        "--expected-source-fingerprint-sha256",
        identity["fingerprint_sha256"],
        "--controller-progress-observer-mode",
        observer,
    ]
    if capture_mechanism:
        evidence_command.append("--capture-mechanism-replay-input")
    sample = {
        "planner_metrics": {
            "measurement_complete": True,
            "collision_probe_available": False,
            "rng_base_seed": 123,
            "rng_seed": 123,
            "shortcut_rng_seed_argument": -124,
        },
        "request": {
            "contact_policy_id": "",
            "contact_policy_resolution": "configured_default",
            "scene_binding": authority["revision"],
            "configured_policy_revision": authority["configured_collision_policy"][
                "revision"
            ],
        },
        "result": {
            "scene_revision": authority["revision"],
            "trajectory_sha256": (
                capture["planning_result"]["trajectory_sha256"]
                if capture is not None
                else _mechanism_capture(mode)["planning_result"][
                    "trajectory_sha256"
                ]
            ),
            "waypoint_count": 96,
            "trajectory_joint_names": ["joint_1", "joint_2"],
            "authoritative_validation": {
                "decision": "passed",
                "event_sequence": 42,
                "joint_limit_and_dynamics_compliance": True,
                "collision_workers_requested": "4",
                "collision_workers_effective": "4",
                "collision_workers_used": "4",
                "collision_parallel_fallback_reason": "",
                "collision_ordered_contact_summary_used": "true",
                "collision_validation_wall_ns": "1000",
            },
        },
    }
    deltas = {name: 0 for name in _VERIFY.ZERO_DELTA_COUNTERS}
    deltas.update(
        {
            "controller_command_progress_samples_total": 30,
            "runtime_collision_ticks_started": 30,
            "runtime_collision_lookahead_jobs_started": 30,
            "validation_collision_jobs_completed": 30,
        }
    )
    runtime_boundary = {
        **{name: 0 for name in _VERIFY.FRESH_BASELINE_METRICS},
        "runtime_collision_lookahead_max_samples": 32,
        "runtime_collision_lookahead_workers_effective": 4,
    }
    return {
        "schema_version": _QUAL.EVIDENCE_SCHEMA,
        "run_id": row_uuid,
        "phase": "rp5_7",
        "source": _source(identity),
        "environment": {
            "command": shlex.join(evidence_command),
            "source_verified_after_run": True,
            "lane": "ros_isaac_container",
            "architecture": "aarch64",
            "cpu_count": 2,
            "cpu_set": "2,3",
            "cpu_governor": "performance",
        },
        "records": [
            {
                "record_type": "live_planning_stage",
                "fixture_id": "isaac_light_drop_approach",
                "passed": True,
                "sample_count": 30,
                "latency_ns": {"p50": 1, "p95": 2, "p99": None, "max": 3},
                "details": {
                    "viewer_stratum": stratum,
                    "measurement_mode": "primary",
                    "viewer_state_websocket_clients": 1 if stratum == "focused" else 0,
                    "viewer_focus_evidence": (
                        "operator_attested_and_state_client_connected"
                        if stratum == "focused"
                        else "server_verified_zero_state_clients"
                    ),
                    "scene_authority_before": authority,
                    "scene_authority_after": authority,
                    "samples": [
                        {**copy.deepcopy(sample), "execution_id": index + 1}
                        for index in range(30)
                    ],
                    "controller_progress_latency": _progress(
                        pristine=pristine, p95_ns=p95_ns, max_ns=max_ns
                    ),
                    "step18_support_contact": _contact(mode, authority),
                    "mechanism_replay_input": capture,
                    "kernel_runtime_metrics": {
                        "before": runtime_boundary,
                        "after": {
                            **runtime_boundary,
                            "controller_command_progress_samples_total": 30,
                            "runtime_collision_ticks_started": 30,
                            "runtime_collision_lookahead_jobs_started": 30,
                            "validation_collision_jobs_completed": 30,
                        },
                        "counter_deltas": deltas,
                    },
                },
            }
        ],
        "inventory": {
            "build_references": {},
            "capabilities": [],
            "workarounds": [],
        },
    }


def _patch(tmp_path: Path) -> Path:
    patch = tmp_path / "baseline-observation.patch"
    payload = subprocess.check_output(
        [
            "git",
            "-C",
            str(_REPO_ROOT),
            "diff",
            "--binary",
            "--full-index",
            "--no-ext-diff",
            # Step 18 is committed now, so reconstruct the observation-only
            # baseline delta from the verifier's pinned pre-Step-18 source.
            # Using HEAD here only worked while the candidate was still an
            # uncommitted working-tree patch and became empty at closure.
            _VERIFY.PINNED_BASELINE_COMMIT,
            "--",
            *sorted(_VERIFY._OBSERVATION_PATCH_PATHS),  # noqa: SLF001
        ]
    )
    patch.write_bytes(payload)
    return patch


def _write_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")


def _matrix(
    tmp_path: Path,
    *,
    candidate_p95: float = 105.0,
    observation_p95: float = 1.0,
) -> Path:
    specs_root = _specs_repository(tmp_path)
    patch = _patch(tmp_path)
    pristine_identity = _VERIFY._source_identity(  # noqa: SLF001
        head_commit=_VERIFY.PINNED_BASELINE_COMMIT,
        head_tree=_VERIFY.PINNED_BASELINE_TREE,
        dirty=False,
        delta_sha256=_VERIFY._delta_sha256(b""),  # noqa: SLF001
    )
    observation_identity = _VERIFY._observation_patch_identity(  # noqa: SLF001
        patch.read_bytes(), _sha256(patch), _sha256(patch)
    )
    candidate_identity = _VERIFY.current_source_identity(_REPO_ROOT)
    baseline_specs = _VERIFY._source_identity(  # noqa: SLF001
        head_commit=_VERIFY.PINNED_SPECS_COMMIT,
        head_tree=_VERIFY.PINNED_SPECS_TREE,
        dirty=False,
        delta_sha256=_VERIFY._delta_sha256(b""),  # noqa: SLF001
    )
    candidate_specs = _SOURCE_ID.source_identity(specs_root)
    baseline_specs_contract = _specs_contract(
        dirty=False, delta_sha256=_VERIFY._delta_sha256(b"")  # noqa: SLF001
    )
    candidate_specs_contract = _SOURCE_ID.specs_contract_identity(specs_root)
    host_root = "/tmp/step18-live-source"
    thor_host_payload = {
        "schema_version": "manyforge.step18_thor_host_identity.v1",
        "hostname": "thor-test-host",
        "architecture": "aarch64",
        "kernel_release": "6.8.12-tegra",
        "device_tree_model": "NVIDIA Jetson AGX Thor Developer Kit",
        "gpus": [
            {
                "name": "NVIDIA Thor",
                "uuid": "GPU-00000000-0000-0000-0000-000000000000",
                "driver_version": "580.00",
            }
        ],
    }
    thor_host_identity = {
        **thor_host_payload,
        "fingerprint_sha256": _canonical_sha256(thor_host_payload),
    }
    runtimes: dict[str, tuple[dict[str, Any], dict[str, Any]]] = {}
    profiles: dict[str, dict[str, Any]] = {}
    for kind in _VERIFY.ROW_KINDS:
        composer = _runtime("manyforge-composer-ros-1", host_root, f"composer-{kind}")
        controller = _runtime(
            "manyforge-isaac-topic-control-1", host_root, f"isaac-controller-{kind}"
        )
        runtimes[kind] = (composer, controller)
        profiles[kind] = _profile(composer, controller, host_root)

    tool_files = [{"path": "scripts/qualification/tool.py", "sha256": "8" * 64}]
    tool_sha = _canonical_sha256(tool_files)
    runner_sha = _sha256(_RUNNER)
    rows: list[dict[str, Any]] = []
    for kind, modes in (
        ("baseline_pristine_timing", (_VERIFY.BASELINE_MODE,)),
        ("baseline_observation", (_VERIFY.BASELINE_MODE,)),
        ("candidate_timing", _VERIFY.CANDIDATE_MODES),
        ("candidate_observation", _VERIFY.CANDIDATE_MODES),
    ):
        for mode in modes:
            for stratum in _VERIFY.VIEWER_STRATA:
                row_id = f"{kind}-{mode}-{stratum}"
                row_uuid = str(uuid.uuid5(uuid.NAMESPACE_URL, row_id))
                identity = {
                    "baseline_pristine_timing": pristine_identity,
                    "baseline_observation": observation_identity,
                    "candidate_timing": candidate_identity,
                    "candidate_observation": candidate_identity,
                }[kind]
                evidence = tmp_path / f"{row_id}.json"
                _write_json(
                    evidence,
                    _artifact(
                        identity=identity,
                        row_uuid=row_uuid,
                        mode=mode,
                        stratum=stratum,
                        pristine=kind == "baseline_pristine_timing",
                        capture_mechanism=kind in _VERIFY.OBSERVATION_ROW_KINDS,
                        p95_ns=(
                            100.0
                            if kind == "baseline_pristine_timing"
                            else observation_p95
                            if kind == "baseline_observation"
                            else candidate_p95
                        ),
                        max_ns=(
                            110.0
                            if kind not in _VERIFY.CANDIDATE_ROW_KINDS
                            else 115.0
                        ),
                    ),
                )
                controller_log = tmp_path / f"{row_id}.log"
                start_marker = f"MANYFORGE_STEP18_ROW {row_uuid} start"
                end_marker = f"MANYFORGE_STEP18_ROW {row_uuid} end"
                controller_log.write_text(
                    "2026-07-20T00:05:00Z "
                    + start_marker
                    + "\n2026-07-20T00:06:00Z controller alive\n"
                    + "2026-07-20T00:10:00Z "
                    + end_marker
                    + "\n",
                    encoding="utf-8",
                )
                composer_runtime, controller_runtime = runtimes[kind]
                started = "2026-07-20T00:01:00.000000Z"
                ended = "2026-07-20T00:11:00.000000Z"
                observer = (
                    "quiescent_repeated"
                    if kind == "baseline_pristine_timing"
                    else "coherent_native"
                )
                command = [
                    "docker",
                    "exec",
                    "manyforge-composer-ros-1",
                    "python3",
                    "collector.py",
                    "--run-id",
                    row_uuid,
                    "--viewer-stratum",
                    stratum,
                    "--measurement-mode",
                    "primary",
                    "--support-contact-mode",
                    mode,
                    "--max-contact-penetration-m",
                    "0.03",
                    "--successful-observations",
                    "30",
                    "--poll-period-s",
                    "0.2",
                    "--phase",
                    "rp5_7",
                    "--expected-source-fingerprint-sha256",
                    identity["fingerprint_sha256"],
                    "--controller-progress-observer-mode",
                    observer,
                ]
                if kind in _VERIFY.OBSERVATION_ROW_KINDS:
                    command.append("--capture-mechanism-replay-input")
                run_metadata = tmp_path / f"{row_id}.run.json"
                metadata = {
                    "schema_version": "manyforge.step18_live_row_run.v2",
                    "row_uuid": row_uuid,
                    "row_kind": kind,
                    "started_at_utc": started,
                    "ended_at_utc": ended,
                    "duration_s": 600.0,
                    "command": shlex.join(command),
                    "collector_stdout": str(evidence),
                    "collector_exit_code": 0,
                    "viewer_stratum": stratum,
                    "support_contact_mode": mode,
                    "max_contact_penetration_m": 0.03,
                    "successful_observations": 30,
                    "poll_period_s": 0.2,
                    "phase": "rp5_7",
                    "controller_progress_observer_mode": observer,
                    "expected_source_fingerprint_sha256": identity[
                        "fingerprint_sha256"
                    ],
                    "observed_source_identity": identity,
                    "specs_source_identity": (
                        candidate_specs
                        if kind in _VERIFY.CANDIDATE_ROW_KINDS
                        else baseline_specs
                    ),
                    "specs_contract_identity": (
                        candidate_specs_contract
                        if kind in _VERIFY.CANDIDATE_ROW_KINDS
                        else baseline_specs_contract
                    ),
                    "composer_runtime": composer_runtime,
                    "controller_runtime": controller_runtime,
                    "runtime_identity_verified_after_run": True,
                    "evidence_path": str(evidence.resolve()),
                    "evidence_sha256": _sha256(evidence),
                    "controller_log_path": str(controller_log.resolve()),
                    "controller_log_sha256": _sha256(controller_log),
                    "controller_log_command": [
                        "docker",
                        "logs",
                        "--timestamps",
                        "--since",
                        started,
                        "--until",
                        ended,
                        controller_runtime["name"],
                    ],
                    "controller_log_markers": {
                        "start": start_marker,
                        "end": end_marker,
                    },
                    "host_repo_root": host_root,
                    "thor_host_identity": thor_host_identity,
                    "qualification_tool_bundle": {
                        "schema_version": "manyforge.step18_qualification_tool_bundle.v1",
                        "files": tool_files,
                        "fingerprint_sha256": tool_sha,
                    },
                    "host_runner": {
                        "path": str(_RUNNER.resolve()),
                        "sha256": runner_sha,
                    },
                }
                _write_json(run_metadata, metadata)
                rows.append(
                    {
                        "id": row_id,
                        "row_uuid": row_uuid,
                        "row_kind": kind,
                        "mode": mode,
                        "viewer_stratum": stratum,
                        "evidence": evidence.name,
                        "evidence_sha256": _sha256(evidence),
                        "controller_log": controller_log.name,
                        "controller_log_sha256": _sha256(controller_log),
                        "run_metadata": run_metadata.name,
                        "run_metadata_sha256": _sha256(run_metadata),
                    }
                )

    parity_rows = []
    for stratum in _VERIFY.VIEWER_STRATA:
        pristine = next(
            row
            for row in rows
            if row["row_kind"] == "baseline_pristine_timing"
            and row["viewer_stratum"] == stratum
        )
        observation = next(
            row
            for row in rows
            if row["row_kind"] == "baseline_observation"
            and row["viewer_stratum"] == stratum
        )
        pristine_evidence = _manifest_value(tmp_path / pristine["evidence"])
        observation_evidence = _manifest_value(tmp_path / observation["evidence"])
        parity_rows.append(
            {
                "viewer_stratum": stratum,
                "pristine_evidence_sha256": pristine["evidence_sha256"],
                "observation_evidence_sha256": observation["evidence_sha256"],
                "pristine_retained_live_output_sha256": (
                    _VERIFY.retained_live_output_sha256(
                        pristine_evidence, f"test.{stratum}.pristine"
                    )
                ),
                "observation_retained_live_output_sha256": (
                    _VERIFY.retained_live_output_sha256(
                        observation_evidence, f"test.{stratum}.observation"
                    )
                ),
            }
        )
    parity = tmp_path / "baseline-observation-parity.json"
    _write_json(
        parity,
        {
            "schema_version": _VERIFY.PARITY_SCHEMA,
            "passed": True,
            "scope": "retained_live_decision_trajectory_outputs",
            "pristine_source_fingerprint_sha256": pristine_identity[
                "fingerprint_sha256"
            ],
            "observation_source_fingerprint_sha256": observation_identity[
                "fingerprint_sha256"
            ],
            "rows": parity_rows,
        },
    )

    baseline_timing_binary = tmp_path / "baseline-timing"
    baseline_observation_binary = tmp_path / "baseline-observation"
    candidate_timing_binary = tmp_path / "candidate-timing"
    candidate_observation_binary = tmp_path / "candidate-observation"
    for binary in (
        baseline_timing_binary,
        baseline_observation_binary,
        candidate_timing_binary,
        candidate_observation_binary,
    ):
        binary.write_bytes((binary.name + "\n").encode())
    baseline_timing_identity = _VERIFY._source_identity(  # noqa: SLF001
        head_commit=_VERIFY.PINNED_BASELINE_COMMIT,
        head_tree=_VERIFY.PINNED_BASELINE_TREE,
        dirty=True,
        delta_sha256="b" * 64,
    )
    baseline_lane = tmp_path / "baseline-lane.json"
    _write_json(
        baseline_lane,
        {
            "schema_version": "manyforge.runtime_collision_baseline_lane.v2",
            "timing_source_identity": baseline_timing_identity,
            "observation_source_identity": observation_identity,
            "timing_binary_sha256": _sha256(baseline_timing_binary),
            "observation_binary_sha256": _sha256(baseline_observation_binary),
        },
    )
    mechanism_records = []
    for row in rows:
        if row["row_kind"] not in _VERIFY.OBSERVATION_ROW_KINDS:
            continue
        evidence_path = tmp_path / row["evidence"]
        evidence_value = _manifest_value(evidence_path)
        details = evidence_value["records"][0]["details"]
        capture = details["mechanism_replay_input"]
        capture_identity = _VERIFY._mechanism_capture_identity(  # noqa: SLF001
            capture,
            details=details,
            field=f"test.{row['id']}.capture",
        )
        capture_path = tmp_path / f"{row['id']}.capture.json"
        capture_path.write_text(
            json.dumps(capture, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        if row["row_kind"] == "baseline_observation":
            execution_specs = (
                (
                    "baseline_decision",
                    "baseline_timing",
                    baseline_timing_binary,
                    "signed_penetration_selective",
                    "baseline_pristine",
                    False,
                ),
                (
                    "baseline_observation",
                    "baseline_observation",
                    baseline_observation_binary,
                    "signed_penetration_selective",
                    "baseline_observation",
                    True,
                ),
            )
        else:
            arm = {
                "signed_penetration": "signed_penetration_selective",
                "binary_contact": "binary_contact",
                "bounded_penetration": "bounded_penetration",
            }[row["mode"]]
            execution_specs = (
                (
                    "baseline_decision",
                    "baseline_timing",
                    baseline_timing_binary,
                    "signed_penetration_selective",
                    "baseline_pristine",
                    False,
                ),
                (
                    "baseline_observation",
                    "baseline_observation",
                    baseline_observation_binary,
                    "signed_penetration_selective",
                    "baseline_observation",
                    True,
                ),
                (
                    "candidate_decision",
                    "candidate_timing",
                    candidate_timing_binary,
                    arm,
                    "current_candidate",
                    False,
                ),
                (
                    "candidate_observation",
                    "candidate_observation",
                    candidate_observation_binary,
                    arm,
                    "current_candidate",
                    True,
                ),
            )
        executions = []
        for role, binary_role, binary, arm, source_role, observed in execution_specs:
            run_id = f"{row['row_uuid']}--{role}"
            artifact_path = tmp_path / f"{row['id']}--{role}.replay.json"
            artifact_value = _replay_artifact(
                capture_identity=capture_identity,
                run_id=run_id,
                arm=arm,
                source_role=source_role,
                observation=observed,
            )
            _write_json(artifact_path, artifact_value)
            capture_container = f"/workspace/test/{capture_path.name}"
            command = [
                "docker",
                "compose",
                "run",
                "--entrypoint",
                f"/workspace/test/{binary.name}",
                "dev",
                "--mechanism-replay-input",
                capture_container,
                "--repo-root",
                "/workspace",
                "--run-id",
                run_id,
                "--source-role",
                source_role,
                "--arm",
                arm,
                "--bounded-penetration-m",
                "0.03",
            ]
            executions.append(
                {
                    "role": role,
                    "binary_role": binary_role,
                    "binary": {"path": str(binary), "sha256": _sha256(binary)},
                    "arm": arm,
                    "source_role": source_role,
                    "run_id": run_id,
                    "capture_container_path": capture_container,
                    "command_argv": command,
                    "stderr_sha256": "0" * 64,
                    "artifact": {
                        "path": str(artifact_path),
                        "sha256": _sha256(artifact_path),
                    },
                    "result_summary": {
                        "measurement_kind": artifact_value["measurement_kind"],
                        "pair_transcript_fingerprint": artifact_value[
                            "pair_transcript_fingerprint"
                        ],
                    },
                }
            )
        mechanism_records.append(
            {
                "observation_row_id": row["id"],
                "row_uuid": row["row_uuid"],
                "row_kind": row["row_kind"],
                "mode": row["mode"],
                "viewer_stratum": row["viewer_stratum"],
                "row_metadata": {
                    "path": str((tmp_path / row["run_metadata"]).resolve()),
                    "sha256": row["run_metadata_sha256"],
                },
                "row_evidence": {
                    "path": str(evidence_path.resolve()),
                    "sha256": row["evidence_sha256"],
                },
                "capture": {
                    "path": str(capture_path.resolve()),
                    "sha256": _sha256(capture_path),
                },
                "capture_identity": capture_identity,
                "executions": executions,
            }
        )
    mechanism_manifest = tmp_path / "mechanism-replay-manifest.json"
    _write_json(
        mechanism_manifest,
        {
            "schema_version": _VERIFY.MECHANISM_REPLAY_MANIFEST_SCHEMA,
            "scope": "exact_ur10e_isaac_live_validation_mechanism_owner",
            "runner": {
                "path": str(
                    _REPO_ROOT
                    / "scripts/qualification/run_step18_live_mechanism_replay.py"
                ),
                "sha256": _sha256(
                    _REPO_ROOT
                    / "scripts/qualification/run_step18_live_mechanism_replay.py"
                ),
            },
            "candidate_source_identity": candidate_identity,
            "candidate_specs_contract_identity": candidate_specs_contract,
            "baseline_lane_metadata": {
                "path": str(baseline_lane),
                "sha256": _sha256(baseline_lane),
            },
            "baseline_timing_source_identity": baseline_timing_identity,
            "baseline_observation_source_identity": observation_identity,
            "dev_image": {
                "reference": "manyforge-ros-jazzy-full",
                "image_id": "sha256:" + "a" * 64,
            },
            "candidate_build_commands_argv": [
                ["docker", "compose", "run", "cmake", "-S", "/workspace/manyforge_roboplan"],
                ["docker", "compose", "run", "cmake", "--build", "/workspace/build/candidate"],
            ],
            "candidate_binaries": {
                "timing": {
                    "path": str(candidate_timing_binary),
                    "sha256": _sha256(candidate_timing_binary),
                },
                "observation": {
                    "path": str(candidate_observation_binary),
                    "sha256": _sha256(candidate_observation_binary),
                },
            },
            "bounded_penetration_m": 0.03,
            "observation_row_ids": [
                record["observation_row_id"] for record in mechanism_records
            ],
            "records": mechanism_records,
        },
    )
    manifest = tmp_path / "manifest.json"
    _write_json(
        manifest,
        {
            "schema_version": _VERIFY.MANIFEST_SCHEMA,
            "expected_baseline_commit": _VERIFY.PINNED_BASELINE_COMMIT,
            "expected_specs_commit": _VERIFY.PINNED_SPECS_COMMIT,
            "expected_candidate_source_identity": candidate_identity,
            "expected_baseline_specs_source_identity": baseline_specs,
            "expected_candidate_specs_source_identity": candidate_specs,
            "expected_baseline_specs_contract_identity": baseline_specs_contract,
            "expected_candidate_specs_contract_identity": candidate_specs_contract,
            "current_candidate_root": str(_REPO_ROOT),
            "current_specs_root": str(specs_root),
            "expected_verifier": {
                "path": str(_SCRIPT.resolve()),
                "sha256": _sha256(_SCRIPT),
            },
            "baseline_observation_patch": patch.name,
            "baseline_observation_patch_sha256": _sha256(patch),
            "baseline_observation_parity": parity.name,
            "baseline_observation_parity_sha256": _sha256(parity),
            "mechanism_replay_manifest": mechanism_manifest.name,
            "mechanism_replay_manifest_sha256": _sha256(mechanism_manifest),
            "expected_host_runner_sha256": runner_sha,
            "expected_qualification_tool_bundle_fingerprint_sha256": tool_sha,
            "expected_thor_host_identity": thor_host_identity,
            "runtime_profiles": profiles,
            "bounded_penetration_m": 0.03,
            "maximum_controller_progress_regression_ratio": 1.10,
            "rows": rows,
        },
    )
    return manifest


def _manifest_value(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _rewrite_manifest(path: Path, value: dict[str, Any]) -> None:
    _write_json(path, value)


def _reseal_row(tmp_path: Path, manifest: dict[str, Any], row: dict[str, Any]) -> None:
    evidence = tmp_path / row["evidence"]
    log = tmp_path / row["controller_log"]
    metadata_path = tmp_path / row["run_metadata"]
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata["evidence_sha256"] = _sha256(evidence)
    metadata["controller_log_sha256"] = _sha256(log)
    _write_json(metadata_path, metadata)
    row["evidence_sha256"] = _sha256(evidence)
    row["controller_log_sha256"] = _sha256(log)
    row["run_metadata_sha256"] = _sha256(metadata_path)


def _reseal_mechanism_manifest(
    manifest_path: Path, manifest: dict[str, Any], mechanism: dict[str, Any]
) -> None:
    mechanism_path = manifest_path.parent / manifest["mechanism_replay_manifest"]
    _write_json(mechanism_path, mechanism)
    manifest["mechanism_replay_manifest_sha256"] = _sha256(mechanism_path)
    _rewrite_manifest(manifest_path, manifest)


def test_step18_live_verifier_accepts_source_bound_sixteen_row_matrix(
    tmp_path: Path,
) -> None:
    result = _VERIFY.verify(_matrix(tmp_path, observation_p95=1.0))

    assert result["live_lane_passed"] is True
    assert result["overall_step18_closure_claimed"] is False
    assert result["failures"] == []
    assert len(result["rows"]) == 16
    assert len(result["comparisons"]) == 6
    assert len(result["artifact_manifest"]) >= 90
    assert result["verifier"] == result["producer_invocation"]["verifier"]
    assert result["producer_invocation"]["arguments"] == [
        "--manifest",
        str(result["manifest"]["path"]),
    ]
    assert all(
        comparison["timing_denominator_source"]
        == "pristine_2bfe30e_unpatched"
        for comparison in result["comparisons"]
    )
    assert result["comparisons"][0]["p95_ratio"] == pytest.approx(1.05)
    assert result["live_mechanism_replay"][
        "all_direct_mechanism_checks_passed"
    ] is True
    assert result["live_mechanism_replay"][
        "all_physical_pair_parity_checks_passed"
    ] is True
    candidate_rows = [
        row for row in result["rows"] if row["row_kind"] == "candidate_timing"
    ]
    assert all(
        row["mechanism_replay_input"] is not None
        for row in result["rows"]
        if row["row_kind"] in _VERIFY.OBSERVATION_ROW_KINDS
    )
    assert all(
        row["mechanism_replay_input"] is None
        for row in result["rows"]
        if row["row_kind"] not in _VERIFY.OBSERVATION_ROW_KINDS
    )
    assert len(
        {
            row["runtime_profile"]["composer_runtime_fingerprint_sha256"]
            for row in candidate_rows
        }
    ) == 1
    assert all(
        "metadata" not in row and "source" not in row and "environment" not in row
        for row in result["rows"]
    )
    for binding in _declared_path_sha_bindings(result):
        path = Path(binding["path"])
        assert path.is_file()
        assert _sha256(path) == binding["sha256"]


def test_step18_live_rejects_non_thor_host_substitution(tmp_path: Path) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    row = manifest["rows"][0]
    metadata_path = tmp_path / row["run_metadata"]
    metadata = _manifest_value(metadata_path)
    metadata["thor_host_identity"]["device_tree_model"] = "generic aarch64"
    _write_json(metadata_path, metadata)
    row["run_metadata_sha256"] = _sha256(metadata_path)
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="Thor host identity changed"):
        _VERIFY.verify(manifest_path)


def test_step18_live_rejects_non_32_sample_lookahead(tmp_path: Path) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    row = manifest["rows"][0]
    evidence_path = tmp_path / row["evidence"]
    evidence = _manifest_value(evidence_path)
    evidence["records"][0]["details"]["kernel_runtime_metrics"]["after"][
        "runtime_collision_lookahead_max_samples"
    ] = 16
    _write_json(evidence_path, evidence)
    _reseal_row(tmp_path, manifest, row)
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="not exactly 32 samples"):
        _VERIFY.verify(manifest_path)


def test_step18_live_rejects_parallel_fallback_under_load(tmp_path: Path) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    row = manifest["rows"][0]
    evidence_path = tmp_path / row["evidence"]
    evidence = _manifest_value(evidence_path)
    evidence["records"][0]["details"]["samples"][0]["result"][
        "authoritative_validation"
    ]["collision_parallel_fallback_reason"] = "insufficient_worker_contexts"
    _write_json(evidence_path, evidence)
    _reseal_row(tmp_path, manifest, row)
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="no-fallback validation"):
        _VERIFY.verify(manifest_path)


def test_step18_live_manifest_generator_derives_rows_profiles_and_parity(
    tmp_path: Path,
) -> None:
    seed_manifest_path = _matrix(tmp_path)
    seed_manifest = _manifest_value(seed_manifest_path)
    generated_manifest = tmp_path / "generated-manifest.json"
    generated_parity = tmp_path / "generated-parity.json"
    result = _GEN.generate(
        argparse.Namespace(
            row_metadata=[tmp_path / row["run_metadata"] for row in seed_manifest["rows"]],
            baseline_observation_patch=(
                tmp_path / seed_manifest["baseline_observation_patch"]
            ),
            mechanism_replay_manifest=(
                tmp_path / seed_manifest["mechanism_replay_manifest"]
            ),
            parity_output=generated_parity,
            output=generated_manifest,
            bounded_penetration_m=0.03,
            maximum_ratio=1.10,
            current_candidate_root=_REPO_ROOT,
            current_specs_root=_specs_repository(tmp_path),
        )
    )

    assert len(result["rows"]) == 16
    assert result["baseline_observation_parity_sha256"] == _sha256(
        generated_parity
    )
    assert _VERIFY.verify(generated_manifest)["live_lane_passed"] is True
    assert _manifest_value(generated_parity)["scope"] == (
        "retained_live_decision_trajectory_outputs"
    )


def test_step18_live_rejects_replay_trajectory_substitution(tmp_path: Path) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    mechanism_path = tmp_path / manifest["mechanism_replay_manifest"]
    mechanism = _manifest_value(mechanism_path)
    execution = mechanism["records"][0]["executions"][0]
    artifact_path = Path(execution["artifact"]["path"])
    artifact = _manifest_value(artifact_path)
    artifact["input"]["trajectory_sha256"] = "f" * 64
    _write_json(artifact_path, artifact)
    execution["artifact"]["sha256"] = _sha256(artifact_path)
    _reseal_mechanism_manifest(manifest_path, manifest, mechanism)

    with pytest.raises(_VERIFY.ValidationError, match="exact live capture"):
        _VERIFY.verify(manifest_path)


def test_step18_live_rejects_candidate_collision_pair_divergence(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    mechanism_path = tmp_path / manifest["mechanism_replay_manifest"]
    mechanism = _manifest_value(mechanism_path)
    record = next(
        item
        for item in mechanism["records"]
        if item["row_kind"] == "candidate_observation"
    )
    for execution in (
        item
        for item in record["executions"]
        if item["role"] in {"candidate_decision", "candidate_observation"}
    ):
        artifact_path = Path(execution["artifact"]["path"])
        artifact = _manifest_value(artifact_path)
        artifact["pair_transcript"][0]["normalized_pairs"].append(
            {
                "body_a": "object/zextra/shape/0",
                "body_b": "robot/ur10e/link/wrist_3_link",
            }
        )
        artifact["pair_transcript_fingerprint"] = "fedcba9876543210"
        execution["result_summary"]["pair_transcript_fingerprint"] = (
            artifact["pair_transcript_fingerprint"]
        )
        _write_json(artifact_path, artifact)
        execution["artifact"]["sha256"] = _sha256(artifact_path)
    _reseal_mechanism_manifest(manifest_path, manifest, mechanism)

    with pytest.raises(_VERIFY.ValidationError, match="collision pairs differ"):
        _VERIFY.verify(manifest_path)


def test_step18_live_rejects_binary_replay_distance_work(tmp_path: Path) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    mechanism_path = tmp_path / manifest["mechanism_replay_manifest"]
    mechanism = _manifest_value(mechanism_path)
    record = next(item for item in mechanism["records"] if item["mode"] == "binary_contact")
    execution = next(
        item
        for item in record["executions"]
        if item["role"] == "candidate_observation"
    )
    artifact_path = Path(execution["artifact"]["path"])
    artifact = _manifest_value(artifact_path)
    artifact["mechanism"]["distance_api_calls"] = 1
    artifact["mechanism"]["distance_logical_pairs"] = 1
    _write_json(artifact_path, artifact)
    execution["artifact"]["sha256"] = _sha256(artifact_path)
    _reseal_mechanism_manifest(manifest_path, manifest, mechanism)

    with pytest.raises(_VERIFY.ValidationError, match="binary mode performed distance"):
        _VERIFY.verify(manifest_path)


def test_step18_live_rejects_bounded_release_only_policy_fields(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    mechanism_path = tmp_path / manifest["mechanism_replay_manifest"]
    mechanism = _manifest_value(mechanism_path)
    record = next(
        item for item in mechanism["records"] if item["mode"] == "bounded_penetration"
    )
    execution = next(
        item
        for item in record["executions"]
        if item["role"] == "candidate_observation"
    )
    artifact_path = Path(execution["artifact"]["path"])
    artifact = _manifest_value(artifact_path)
    artifact["policy_contract"]["support_contacts"][0][
        "require_separation_by_end"
    ] = True
    _write_json(artifact_path, artifact)
    execution["artifact"]["sha256"] = _sha256(artifact_path)
    _reseal_mechanism_manifest(manifest_path, manifest, mechanism)

    with pytest.raises(_VERIFY.ValidationError, match="canonical bounded penetration"):
        _VERIFY.verify(manifest_path)


@pytest.mark.parametrize(
    "mutation",
    (
        "tracked_regular",
        "tracked_symlink",
        "untracked_regular",
        "untracked_symlink",
        "evidence_only",
    ),
)
def test_specs_contract_identity_module_live_runner_and_cli_are_identical(
    tmp_path: Path, mutation: str
) -> None:
    repo = tmp_path / "specs"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    subprocess.run(
        ["git", "-C", str(repo), "config", "user.email", "test@example.invalid"],
        check=True,
    )
    subprocess.run(
        ["git", "-C", str(repo), "config", "user.name", "Step18 Test"],
        check=True,
    )
    (repo / "tracked.txt").write_text("before\n", encoding="utf-8")
    (repo / "target-a").write_text("a\n", encoding="utf-8")
    (repo / "target-b").write_text("b\n", encoding="utf-8")
    (repo / "tracked-link").symlink_to("target-a")
    subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
    subprocess.run(
        ["git", "-C", str(repo), "commit", "-q", "-m", "fixture"], check=True
    )
    if mutation == "tracked_regular":
        (repo / "tracked.txt").write_text("after\n", encoding="utf-8")
    elif mutation == "tracked_symlink":
        (repo / "tracked-link").unlink()
        (repo / "tracked-link").symlink_to("target-b")
    elif mutation == "untracked_regular":
        (repo / "untracked.txt").write_text("payload\n", encoding="utf-8")
    elif mutation == "untracked_symlink":
        (repo / "untracked-link").symlink_to("target-a")
    else:
        evidence = repo / _SOURCE_ID.SPECS_CONTRACT_EXCLUDED_PATH
        evidence.parent.mkdir(parents=True)
        evidence.write_text("post-run evidence\n", encoding="utf-8")

    module_identity = _SOURCE_ID.specs_contract_identity(repo)
    live_runner_identity = _RUN._specs_contract_identity(repo)  # noqa: SLF001
    cli = json.loads(
        subprocess.check_output(
            [
                sys.executable,
                str(_REPO_ROOT / "scripts/qualification/source_identity.py"),
                "--repo-root",
                str(repo),
                "--include-specs-contract-identity",
            ],
            text=True,
        )
    )

    assert live_runner_identity == module_identity
    assert cli["specs_contract_identity"] == module_identity
    assert cli["source_identity"] == _SOURCE_ID.source_identity(repo)
    assert cli["source_identity"] == _QUAL._manyforge_source_identity(  # noqa: SLF001
        repo
    )
    assert module_identity["contract_dirty"] is (mutation != "evidence_only")


@pytest.mark.parametrize("mutation", ("omit_reload", "dirty_fresh_baseline"))
def test_step18_live_verifier_requires_reload_bound_fresh_metrics(
    tmp_path: Path, mutation: str
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    row = next(
        item for item in manifest["rows"] if item["row_kind"] == "candidate_timing"
    )
    evidence_path = tmp_path / row["evidence"]
    evidence = _manifest_value(evidence_path)
    if mutation == "omit_reload":
        evidence["source"].pop("artifact_reload")
        match = "disk-backed program/deployment reload"
    else:
        evidence["records"][0]["details"]["kernel_runtime_metrics"]["before"][
            "validation_collision_jobs_completed"
        ] = 1
        match = "fresh metric baseline"
    _write_json(evidence_path, evidence)
    _reseal_row(tmp_path, manifest, row)
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match=match):
        _VERIFY.verify(manifest_path)


def test_step18_live_verifier_rejects_progress_regression_and_deadline_miss(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path, candidate_p95=111.0)
    manifest = _manifest_value(manifest_path)
    candidate = next(
        row for row in manifest["rows"] if row["row_kind"] == "candidate_timing"
    )
    candidate_log = tmp_path / candidate["controller_log"]
    candidate_log.write_text(
        candidate_log.read_text(encoding="utf-8")
        + "Overrun detected! missed cycles : 2\n",
        encoding="utf-8",
    )
    _reseal_row(tmp_path, manifest, candidate)
    _rewrite_manifest(manifest_path, manifest)

    result = _VERIFY.verify(manifest_path)

    assert result["live_lane_passed"] is False
    assert any("controller deadline misses=2" in item for item in result["failures"])
    assert any("controller progress ratio" in item for item in result["failures"])


@pytest.mark.parametrize(
    ("field", "value", "match"),
    [
        ("expected_baseline_commit", "a" * 40, "pinned 2bfe30e"),
        ("expected_specs_commit", "b" * 40, "pinned 67c2444"),
    ],
)
def test_step18_live_verifier_rejects_manifest_substitution(
    tmp_path: Path, field: str, value: str, match: str
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    manifest[field] = value
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match=match):
        _VERIFY.verify(manifest_path)


def test_step18_live_verifier_rejects_candidate_commit_substitution(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    identity = manifest["expected_candidate_source_identity"]
    identity["head_commit"] = "a" * 40
    identity["fingerprint_sha256"] = _QUAL._manyforge_source_fingerprint_sha256(  # noqa: SLF001
        head_commit=identity["head_commit"],
        head_tree=identity["head_tree"],
        dirty=True,
        delta_sha256=identity["delta_sha256"],
    )
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="frozen 2bfe30e"):
        _VERIFY.verify(manifest_path)


def test_step18_live_verifier_rejects_observation_patch_scope_change(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    patch = tmp_path / manifest["baseline_observation_patch"]
    patch.write_bytes(
        patch.read_bytes()
        + b"diff --git a/unrelated.txt b/unrelated.txt\n"
        + b"index 0000000000000000000000000000000000000000..1111111111111111111111111111111111111111 100644\n"
    )
    manifest["baseline_observation_patch_sha256"] = _sha256(patch)
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="patch scope changed"):
        _VERIFY.verify(manifest_path)


def test_step18_live_verifier_rejects_mixed_row_uuid_even_when_rehashed(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    row = manifest["rows"][0]
    metadata_path = tmp_path / row["run_metadata"]
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata["row_uuid"] = str(uuid.uuid4())
    _write_json(metadata_path, metadata)
    row["run_metadata_sha256"] = _sha256(metadata_path)
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="row binding changed"):
        _VERIFY.verify(manifest_path)


def test_step18_live_verifier_rejects_loaded_native_or_image_substitution(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    manifest["runtime_profiles"]["candidate_timing"][
        "composer_native_artifacts_sha256"
    ] = "0" * 64
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="loaded native artifacts changed"):
        _VERIFY.verify(manifest_path)


def test_step18_live_verifier_rejects_explicit_policy_override(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    row = next(item for item in manifest["rows"] if item["row_kind"] == "candidate_timing")
    evidence_path = tmp_path / row["evidence"]
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    request = evidence["records"][0]["details"]["samples"][0]["request"]
    request["contact_policy_id"] = "unexpected-explicit-policy"
    request["contact_policy_resolution"] = "explicit"
    _write_json(evidence_path, evidence)
    _reseal_row(tmp_path, manifest, row)
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="configured-default Step 18 policy"):
        _VERIFY.verify(manifest_path)


def test_step18_live_verifier_rejects_policy_revision_drift(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    row = next(item for item in manifest["rows"] if item["row_kind"] == "candidate_timing")
    evidence_path = tmp_path / row["evidence"]
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    evidence["records"][0]["details"]["scene_authority_after"][
        "configured_collision_policy"
    ]["revision"]["revision_id"] = 10
    _write_json(evidence_path, evidence)
    _reseal_row(tmp_path, manifest, row)
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="changed the applied collision policy"):
        _VERIFY.verify(manifest_path)


def test_step18_live_verifier_rejects_unmatched_input_fingerprint(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    row = next(item for item in manifest["rows"] if item["row_kind"] == "candidate_timing")
    evidence_path = tmp_path / row["evidence"]
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    evidence["source"]["matched_input_sha256"] = "0" * 64
    _write_json(evidence_path, evidence)
    _reseal_row(tmp_path, manifest, row)
    _rewrite_manifest(manifest_path, manifest)

    result = _VERIFY.verify(manifest_path)
    assert result["live_lane_passed"] is False
    assert "canonical program/deployment input differs across matched rows" in result[
        "failures"
    ]


def test_step18_live_verifier_rejects_empty_bound_controller_log(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    row = manifest["rows"][0]
    (tmp_path / row["controller_log"]).write_text("", encoding="utf-8")
    _reseal_row(tmp_path, manifest, row)
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match="marker capture is incomplete"):
        _VERIFY.verify(manifest_path)


def test_step18_live_verifier_rejects_toctou_after_buffered_parse(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    victim = tmp_path / manifest["rows"][0]["evidence"]
    original = _VERIFY._FileTracker.assert_unchanged  # noqa: SLF001
    mutated = False

    def mutate_before_final_rehash(tracker: Any) -> None:
        nonlocal mutated
        if not mutated:
            victim.write_bytes(victim.read_bytes() + b"\n")
            mutated = True
        original(tracker)

    monkeypatch.setattr(
        _VERIFY._FileTracker,  # noqa: SLF001
        "assert_unchanged",
        mutate_before_final_rehash,
    )

    with pytest.raises(_VERIFY.ValidationError, match="changed during verification"):
        _VERIFY.verify(manifest_path)


def test_step18_live_generator_rejects_toctou_after_buffered_parse(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    seed_manifest_path = _matrix(tmp_path)
    seed_manifest = _manifest_value(seed_manifest_path)
    victim = tmp_path / seed_manifest["rows"][0]["evidence"]
    original = _GEN._InputTracker.assert_unchanged  # noqa: SLF001
    mutated = False

    def mutate_before_final_rehash(tracker: Any) -> None:
        nonlocal mutated
        if not mutated:
            victim.write_bytes(victim.read_bytes() + b"\n")
            mutated = True
        original(tracker)

    monkeypatch.setattr(
        _GEN._InputTracker,  # noqa: SLF001
        "assert_unchanged",
        mutate_before_final_rehash,
    )

    with pytest.raises(RuntimeError, match="changed during generation"):
        _GEN.generate(
            argparse.Namespace(
                row_metadata=[
                    tmp_path / row["run_metadata"]
                    for row in seed_manifest["rows"]
                ],
                baseline_observation_patch=(
                    tmp_path / seed_manifest["baseline_observation_patch"]
                ),
                mechanism_replay_manifest=(
                    tmp_path / seed_manifest["mechanism_replay_manifest"]
                ),
                parity_output=tmp_path / "generated-parity.json",
                output=tmp_path / "generated-manifest.json",
                bounded_penetration_m=0.03,
                maximum_ratio=1.10,
                current_candidate_root=_REPO_ROOT,
                current_specs_root=_specs_repository(tmp_path),
            )
        )
    assert not (tmp_path / "generated-parity.json").exists()
    assert not (tmp_path / "generated-manifest.json").exists()


@pytest.mark.parametrize("identity_kind", ("candidate", "specs_contract"))
def test_step18_live_verifier_rechecks_current_identities_at_end(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, identity_kind: str
) -> None:
    manifest_path = _matrix(tmp_path)
    if identity_kind == "candidate":
        original = _VERIFY.current_source_identity
        calls = 0

        def changed_on_final(root: Path) -> dict[str, Any]:
            nonlocal calls
            calls += 1
            value = copy.deepcopy(original(root))
            if calls > 1:
                value["delta_sha256"] = "0" * 64
            return value

        monkeypatch.setattr(_VERIFY, "current_source_identity", changed_on_final)
        match = "candidate changed during verification"
    else:
        original_contract = _VERIFY.current_specs_contract_identity
        calls = 0

        def contract_changed_on_final(root: Path) -> dict[str, Any]:
            nonlocal calls
            calls += 1
            value = copy.deepcopy(original_contract(root))
            if calls > 1:
                value["contract_delta_sha256"] = "0" * 64
            return value

        monkeypatch.setattr(
            _VERIFY,
            "current_specs_contract_identity",
            contract_changed_on_final,
        )
        match = "Specs contract changed during verification"

    with pytest.raises(_VERIFY.ValidationError, match=match):
        _VERIFY.verify(manifest_path)


@pytest.mark.parametrize("substitution", ("path", "hash"))
def test_step18_live_verifier_rejects_verifier_substitution(
    tmp_path: Path, substitution: str
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    if substitution == "path":
        substitute = tmp_path / "substituted-live-verifier.py"
        substitute.write_bytes(_SCRIPT.read_bytes())
        manifest["expected_verifier"] = {
            "path": str(substitute),
            "sha256": _sha256(substitute),
        }
        match = "path substitution"
    else:
        manifest["expected_verifier"]["sha256"] = "0" * 64
        match = "hash substitution"
    _rewrite_manifest(manifest_path, manifest)

    with pytest.raises(_VERIFY.ValidationError, match=match):
        _VERIFY.verify(manifest_path)


def test_step18_live_cli_rejects_unignored_output(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    output = tmp_path / "live-summary.json"

    assert (
        _VERIFY.main(
            ["--manifest", str(manifest_path), "--output", str(output)]
        )
        == 2
    )
    assert not output.exists()


def test_step18_live_cli_writes_only_ignored_output_without_source_delta(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    output = (
        _REPO_ROOT
        / ".manyforge-var/qualification"
        / f"pytest-live-summary-{uuid.uuid4()}.json"
    )
    before = _SOURCE_ID.source_identity(_REPO_ROOT)
    try:
        assert (
            _VERIFY.main(
                ["--manifest", str(manifest_path), "--output", str(output)]
            )
            == 0
        )
        assert _manifest_value(output)["live_lane_passed"] is True
        assert _SOURCE_ID.source_identity(_REPO_ROOT) == before
    finally:
        output.unlink(missing_ok=True)


def test_step18_live_cli_removes_temporary_output_on_postwrite_input_drift(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest_path = _matrix(tmp_path)
    result = _VERIFY.verify(manifest_path)
    output = (
        _REPO_ROOT
        / ".manyforge-var/qualification"
        / f"pytest-live-publication-drift-{uuid.uuid4()}.json"
    )
    original_payload = manifest_path.read_bytes()
    original_recheck = _VERIFY._assert_live_result_bindings_current  # noqa: SLF001

    def drift_before_publication(value: dict[str, Any]) -> None:
        manifest_path.write_bytes(original_payload + b"\n")
        original_recheck(value)

    monkeypatch.setattr(_VERIFY, "verify", lambda *args, **kwargs: result)
    monkeypatch.setattr(
        _VERIFY,
        "_assert_live_result_bindings_current",
        drift_before_publication,
    )
    try:
        assert (
            _VERIFY.main(
                ["--manifest", str(manifest_path), "--output", str(output)]
            )
            == 2
        )
        assert not output.exists()
        assert not list(output.parent.glob(f".{output.name}.tmp-*"))
    finally:
        manifest_path.write_bytes(original_payload)
        output.unlink(missing_ok=True)


def test_step18_live_generator_cli_rejects_unignored_outputs(
    tmp_path: Path,
) -> None:
    manifest_path = _matrix(tmp_path)
    manifest = _manifest_value(manifest_path)
    argv: list[str] = []
    for row in manifest["rows"]:
        argv.extend(("--row-metadata", str(tmp_path / row["run_metadata"])))
    argv.extend(
        (
                "--baseline-observation-patch",
                str(tmp_path / manifest["baseline_observation_patch"]),
                "--mechanism-replay-manifest",
                str(tmp_path / manifest["mechanism_replay_manifest"]),
                "--parity-output",
            str(tmp_path / "generated-parity.json"),
            "--output",
            str(tmp_path / "generated-manifest.json"),
            "--current-candidate-root",
            str(_REPO_ROOT),
            "--current-specs-root",
            str(_specs_repository(tmp_path)),
        )
    )

    assert _GEN.main(argv) == 2
    assert not (tmp_path / "generated-parity.json").exists()
    assert not (tmp_path / "generated-manifest.json").exists()
