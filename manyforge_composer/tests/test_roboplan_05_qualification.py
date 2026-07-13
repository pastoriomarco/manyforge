"""Regression tests for the version-neutral RoboPlan transition artifacts."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import pytest
import yaml


_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "scripts/qualification/roboplan_05_qualification.py"
_SPEC = importlib.util.spec_from_file_location("roboplan_05_qualification", _SCRIPT)
assert _SPEC is not None and _SPEC.loader is not None
_MODULE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MODULE)
_RUNNER_SCRIPT = (
    _REPO_ROOT / "scripts/qualification/run_roboplan_05_offline_baseline.py"
)
_RUNNER_SPEC = importlib.util.spec_from_file_location(
    "run_roboplan_05_offline_baseline", _RUNNER_SCRIPT
)
assert _RUNNER_SPEC is not None and _RUNNER_SPEC.loader is not None
_RUNNER = importlib.util.module_from_spec(_RUNNER_SPEC)
_RUNNER_SPEC.loader.exec_module(_RUNNER)
_CAPTURE_SCRIPT = (
    _REPO_ROOT / "scripts/qualification/capture_isaac_drop_approach.py"
)
_CAPTURE_SPEC = importlib.util.spec_from_file_location(
    "capture_isaac_drop_approach", _CAPTURE_SCRIPT
)
assert _CAPTURE_SPEC is not None and _CAPTURE_SPEC.loader is not None
_CAPTURE = importlib.util.module_from_spec(_CAPTURE_SPEC)
_CAPTURE_SPEC.loader.exec_module(_CAPTURE)
_LIVE_SCRIPT = (
    _REPO_ROOT / "scripts/qualification/run_roboplan_05_live_baseline.py"
)
_LIVE_SPEC = importlib.util.spec_from_file_location(
    "run_roboplan_05_live_baseline", _LIVE_SCRIPT
)
assert _LIVE_SPEC is not None and _LIVE_SPEC.loader is not None
_LIVE = importlib.util.module_from_spec(_LIVE_SPEC)
_LIVE_SPEC.loader.exec_module(_LIVE)


def test_checked_in_corpus_has_required_sample_sizes_and_unique_links() -> None:
    corpus = _REPO_ROOT / "benchmarks/roboplan_05/corpus/corpus-v1.json"
    summary = _MODULE.validate_corpus(corpus, allow_pending_capture=True)
    assert summary == {
        "schema_version": "manyforge.roboplan_transition_corpus.v1",
        "models": 3,
        "collision_fixtures": 6,
        "revision_fixtures": 5,
        "planning_fixtures": 3,
        "live_cases": 1,
        "pending_capture_allowed": True,
    }
    value = json.loads(corpus.read_text(encoding="utf-8"))
    assert value["models"]["lite6_gripper"]["group_name"] == "lite6"


def test_isaac_live_baseline_plays_once_and_reset_owns_loop_replay() -> None:
    program = yaml.safe_load(
        (
            _REPO_ROOT / "examples/isaac_manipulator_scene_light.program.yaml"
        ).read_text(encoding="utf-8")
    )
    deployment = yaml.safe_load(
        (
            _REPO_ROOT
            / "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml"
        ).read_text(encoding="utf-8")
    )

    root = program["tree"]
    assert root["id"] == "sequence"
    assert root["name"] == "run_sequence"
    assert [child["id"] for child in root["children"]] == [
        "set_simulation_state",
        "repeat",
    ]
    play_once, repeat = root["children"]
    assert play_once["params"]["state"] == "playing"
    assert repeat["params"]["num_cycles"] == -1

    def descendants(node: dict[str, object]) -> list[dict[str, object]]:
        children = list(node.get("children") or [])
        return children + [nested for child in children for nested in descendants(child)]

    repeated_nodes = descendants(repeat)
    assert all(node.get("id") != "set_simulation_state" for node in repeated_nodes)
    assert sum(node.get("id") == "reset_simulation" for node in repeated_nodes) == 1
    assert (
        deployment["simulation_control"]["reset"]["resume_play_after_reset"]
        is True
    )


def test_corpus_rejects_pooled_or_under_sampled_live_strata(tmp_path: Path) -> None:
    source = _REPO_ROOT / "benchmarks/roboplan_05/corpus/corpus-v1.json"
    corpus = json.loads(source.read_text(encoding="utf-8"))
    corpus["live_cases"][0]["viewer_strata"] = ["focused"]
    corpus["live_cases"][0]["successful_observations_per_stratum"] = 10
    path = tmp_path / "corpus.json"
    path.write_text(json.dumps(corpus), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="strata separate"):
        _MODULE.validate_corpus(path, allow_pending_capture=True)


def test_corpus_requires_captured_replay_before_final_validation(
    tmp_path: Path,
) -> None:
    source = _REPO_ROOT / "benchmarks/roboplan_05/corpus/corpus-v1.json"
    path = tmp_path / "corpus.json"
    path.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="capture is missing"):
        _MODULE.validate_corpus(path)


def test_evidence_validator_rejects_non_boolean_passed(tmp_path: Path) -> None:
    evidence = {
        "schema_version": "manyforge.roboplan_transition_evidence.v1",
        "run_id": "run-1",
        "phase": "rp5_0",
        "source": {},
        "environment": {"command": "python qualification.py"},
        "records": [
            {
                "record_type": "inventory",
                "fixture_id": "versions",
                "passed": "yes",
            }
        ],
    }
    path = tmp_path / "evidence.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="passed must be boolean"):
        _MODULE.validate_evidence(path)


def test_evidence_validator_rejects_undersampled_clean_source_baseline(
    tmp_path: Path,
) -> None:
    evidence = {
        "schema_version": "manyforge.roboplan_transition_evidence.v1",
        "run_id": "formal-run",
        "phase": "rp5_0",
        "source": {
            "manyforge_dirty": False,
            "manyforge_commit": "a" * 40,
            "roboplan_version": "0.4.0",
            "roboplan_commit": "b" * 40,
            "pinocchio_version": "4.0.0",
        },
        "environment": {"command": "python qualification.py"},
        "records": [
            {
                "record_type": "collision_query",
                "fixture_id": "cell",
                "query_class": "binary_complete",
                "passed": True,
                "sample_count": 9999,
                "latency_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
                "cpu_time_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
                "allocation_count": {
                    "p50": 0,
                    "p95": 0,
                    "p99": 0,
                    "max": 0,
                },
            }
        ],
        "inventory": {
            "build_references": {},
            "capabilities": [],
            "workarounds": [],
        },
    }
    path = tmp_path / "undersampled.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="at least 10000"):
        _MODULE.validate_evidence(path)


def _formal_live_evidence() -> dict:
    authority = {
        "revision": {"revision_id": 7, "content_hash": "scene-seven"},
        "transaction_sequence": 11,
        "scene_admission_epoch": 3,
        "topology_epoch": 5,
        "policy_epoch": 2,
        "configured_collision_policy": {
            "policy_id": "default_free_space",
            "revision": {"revision_id": 1, "content_hash": "policy-one"},
        },
    }
    sample = {
        "planner_metrics": {
            "measurement_complete": True,
            "collision_probe_available": False,
        },
        "request": {
            "contact_policy_id": "",
            "contact_policy_resolution": "configured_default",
            "scene_binding": {
                "revision_id": 7,
                "content_hash": "scene-seven",
                "projection_lease_id": "lease-seven",
            },
        },
        "result": {
            "scene_revision": {
                "revision_id": 7,
                "content_hash": "scene-seven",
            }
        },
    }
    return {
        "schema_version": "manyforge.roboplan_transition_evidence.v1",
        "run_id": "formal-live",
        "phase": "rp5_0",
        "source": {
            "manyforge_dirty": False,
            "manyforge_commit": "a" * 40,
            "roboplan_version": "0.4.0",
            "roboplan_commit": "b" * 40,
            "pinocchio_version": "4.0.0",
            "program": {
                "name": "isaac_manipulator_scene_light",
                "path": "examples/isaac_manipulator_scene_light.program.yaml",
                "sha256": "c" * 64,
            },
            "deployment": {
                "name": "isaac_manipulator_scene_light_ros2_control",
                "path": (
                    "examples/"
                    "isaac_manipulator_scene_light_ros2_control.deployment.yaml"
                ),
                "sha256": "d" * 64,
            },
        },
        "environment": {
            "command": "python run_live.py --output live.json",
            "source_verified_after_run": True,
        },
        "records": [
            {
                "record_type": "live_planning_stage",
                "fixture_id": "isaac_light_drop_approach",
                "passed": True,
                "sample_count": 30,
                "latency_ns": {"p50": 1, "p95": 2, "p99": None, "max": 3},
                "details": {
                    "viewer_stratum": "disconnected",
                    "measurement_mode": "primary",
                    "scene_authority_before": authority,
                    "scene_authority_after": authority,
                    "samples": [sample for _ in range(30)],
                },
            }
        ],
        "inventory": {
            "build_references": {},
            "capabilities": [],
            "workarounds": [],
        },
    }


def test_evidence_validator_requires_reproducible_formal_live_provenance(
    tmp_path: Path,
) -> None:
    evidence = _formal_live_evidence()
    path = tmp_path / "formal-live.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")
    assert _MODULE.validate_evidence(path)["records"] == 1

    del evidence["environment"]["command"]
    path.write_text(json.dumps(evidence), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="environment.command"):
        _MODULE.validate_evidence(path)


def test_evidence_validator_rejects_live_scene_revision_substitution(
    tmp_path: Path,
) -> None:
    evidence = _formal_live_evidence()
    evidence["records"][0]["details"]["samples"][0]["result"][
        "scene_revision"
    ]["revision_id"] = 8
    path = tmp_path / "scene-substitution.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="changed scene revision"):
        _MODULE.validate_evidence(path)


def test_qualification_rejects_source_changes_during_measurement() -> None:
    before = {"source": {"manyforge_commit": "a" * 40, "manyforge_dirty": False}}
    _MODULE.require_source_inventory_stable(before, json.loads(json.dumps(before)))

    after = json.loads(json.dumps(before))
    after["source"]["manyforge_dirty"] = True
    with pytest.raises(RuntimeError, match="source identity changed"):
        _MODULE.require_source_inventory_stable(before, after)


def test_engine_matrix_translation_preserves_revision_and_validation_evidence() -> None:
    records = _RUNNER._engine_matrix_records(
        {
            "fixtures": [
                {
                    "fixture": "support",
                    "passed": True,
                    "support_contact_fallback_verified": True,
                    "runtime_query": {
                        "revision_change": {
                            "p50_us": 1.0,
                            "p95_us": 2.0,
                            "p99_us": 3.0,
                            "max_us": 4.0,
                            "allocations": 8,
                        }
                    },
                    "validation": {
                        "enabled": True,
                        "runs": 9,
                        "decision_match": True,
                    },
                    "noncollision_validation": {
                        "enabled": True,
                        "runs": 9,
                        "p50_us": 5.0,
                        "p95_us": 6.0,
                        "p99_us": 7.0,
                        "max_us": 8.0,
                    },
                }
            ]
        }
    )
    assert [record["record_type"] for record in records] == [
        "collision_revision",
        "trajectory_validation",
    ]
    assert records[0]["latency_ns"]["p95"] == 2000.0
    assert records[1]["details"]["support_contact_fallback_verified"] is True


def test_live_summary_omits_p99_for_thirty_sample_stratum() -> None:
    summary = _LIVE._summary(list(range(1, 31)))
    assert summary["p50"] == 15.5
    assert summary["p95"] == pytest.approx(28.55)
    assert summary["p99"] is None
    assert summary["max"] == 30.0


def test_live_planner_records_pair_by_serial_admission_and_move_identity() -> None:
    events = [
        {
            "type": "cache_miss",
            "execution_id": 7,
            "details": {"move_id": "pick_retreat"},
        },
        {"type": "plan_started", "execution_id": 7},
        {
            "type": "cache_miss",
            "execution_id": 8,
            "details": {"move_id": "drop_approach"},
        },
        {"type": "plan_started", "execution_id": 8},
    ]
    records = [
        {"sequence": 1, "request": {"move_id": "pick_retreat"}},
        {"sequence": 2, "request": {"move_id": "drop_approach"}},
    ]
    paired = _LIVE._pair_planner_records(events, records)
    assert paired[8]["sequence"] == 2

    records[1]["request"]["move_id"] = "wrong"
    with pytest.raises(RuntimeError, match="admission order"):
        _LIVE._pair_planner_records(events, records)


def test_live_success_requires_behavior_consumption_of_kernel_completion() -> None:
    events = [
        {
            "type": "cache_miss",
            "execution_id": 4,
            "details": {"move_id": "drop_approach"},
        },
        *(
            {"type": event_type, "execution_id": 4}
            for event_type in (
                "plan_started",
                "plan_completed",
                "validation_started",
                "validation_passed",
                "execute_started",
                "execute_completed",
            )
        ),
    ]
    assert _LIVE._successful_drop_execution_ids(events) == []
    events.append({"type": "acknowledge_completed", "execution_id": 4})
    assert _LIVE._successful_drop_execution_ids(events) == [4]


def test_live_sample_reports_true_behavior_node_envelope() -> None:
    timestamps = {
        "plan_started": 110,
        "plan_completed": 200,
        "validation_started": 210,
        "validation_passed": 400,
        "execute_started": 410,
        "execute_completed": 700,
    }
    events = [
        {
            "type": "state_transition",
            "execution_id": 4,
            "to_state": "Planning",
            "timestamp_ns": 100,
        },
        *(
            {
                "type": event_type,
                "execution_id": 4,
                "timestamp_ns": timestamp,
                "sequence": 50 if event_type == "validation_passed" else 0,
            }
            for event_type, timestamp in timestamps.items()
        ),
    ]
    planner_record = {
        "sequence": 1,
        "request": {
            "contact_policy_id": "default_free_space",
            "scene_binding": {
                "revision_id": 7,
                "content_hash": "scene-7",
                "projection_lease_id": "lease-7",
            },
        },
        "metrics": {
            "total_duration_ns": 80,
            "measurement_complete": True,
            "collision_probe_available": False,
            "succeeded": True,
            "waypoint_count": 2,
            "trajectory_duration_s": 1.0,
            "rng_base_seed": 12345,
            "rng_seed": 12345,
            "shortcut_rng_seed_argument": -12346,
        },
        "result": {
            "scene_revision": {
                "revision_id": 7,
                "content_hash": "scene-7",
            },
            "trajectory": {
                "joint_names": ["j1"],
                "waypoints": [
                    {"time_from_start_s": 0.0, "positions_rad": [0.0]},
                    {"time_from_start_s": 1.0, "positions_rad": [0.5]},
                ],
            },
        },
        "behavior_node": {
            "terminal_phase": "succeeded",
            "started_monotonic_ns": 90,
            "completed_monotonic_ns": 710,
            "thread_cpu_duration_ns": 12,
        },
    }

    class _Sampler:
        @staticmethod
        def interval_bounds(start_ns, end_ns):
            return {
                "estimate_ns": end_ns - start_ns,
                "lower_bound_ns": end_ns - start_ns,
                "upper_bound_ns": end_ns - start_ns,
                "uncertainty_ns": 0,
                "start_bracket_ns": 1,
                "end_bracket_ns": 1,
            }

    sample = _LIVE._sample_payload(
        4, events, planner_record, _Sampler(), "primary"
    )
    assert sample["wall_duration_ns"]["behavior_pre_admission"] == 10
    assert sample["wall_duration_ns"]["behavior_post_kernel"] == 10
    assert sample["wall_duration_ns"]["total_behavior_node"] == 620
    assert sample["process_cpu"]["behavior_node"]["estimate_ns"] == 620
    assert sample["result"]["joint_space_path_length_rad"] == 0.5
    assert len(sample["result"]["trajectory_sha256"]) == 64
    assert sample["result"]["rng_seed"] == 12345
    assert sample["request"] == {
        "contact_policy_id": "default_free_space",
        "contact_policy_resolution": "explicit",
        "scene_binding": {
            "revision_id": 7,
            "content_hash": "scene-7",
            "projection_lease_id": "lease-7",
        },
    }
    assert sample["result"]["authoritative_validation"] == {
        "decision": "passed",
        "event_sequence": 50,
        "joint_limit_and_dynamics_compliance": True,
    }

    planner_record["result"]["scene_revision"]["revision_id"] = 8
    with pytest.raises(RuntimeError, match="changed its bound scene revision"):
        _LIVE._sample_payload(4, events, planner_record, _Sampler(), "primary")
    planner_record["result"]["scene_revision"]["revision_id"] = 7

    planner_record["metrics"]["collision_probe_available"] = True
    planner_record["metrics"]["total_collision_query_count"] = 4
    with pytest.raises(RuntimeError, match="primary latency was contaminated"):
        _LIVE._sample_payload(4, events, planner_record, _Sampler(), "primary")

    planner_record["metrics"]["collision_probe_available"] = False
    with pytest.raises(RuntimeError, match="has no planner collision probe"):
        _LIVE._sample_payload(
            4,
            events,
            planner_record,
            _Sampler(),
            "collision_probe_attribution",
        )


def test_live_event_sequence_tracker_accepts_cross_batch_producer_reordering() -> None:
    tracker = _LIVE._KernelSequenceTracker(1258)

    tracker.observe([{"sequence": 1260}, {"sequence": 1261}])
    assert tracker.committed_sequence == 1258
    assert tracker.settled is False

    tracker.observe([{"sequence": 1259}])
    assert tracker.committed_sequence == 1261
    assert tracker.settled is True


def test_live_event_sequence_gap_is_fail_closed_at_quiescence() -> None:
    tracker = _LIVE._KernelSequenceTracker(11)
    tracker.observe([{"sequence": 13}])

    with pytest.raises(
        RuntimeError,
        match="expected 12, next available 13, missing through 12",
    ):
        tracker.require_settled(reason="test quiescence")


def test_live_event_sequence_tracker_rejects_duplicate_or_regressed_values() -> None:
    tracker = _LIVE._KernelSequenceTracker(10)
    tracker.observe([{"sequence": 11}])
    with pytest.raises(RuntimeError, match="duplicate or regressed"):
        tracker.observe([{"sequence": 11}])


def test_live_evidence_writes_are_atomic_and_never_clobber(tmp_path: Path) -> None:
    path = tmp_path / "live.json"
    first = {"run_id": "first"}
    second = {"run_id": "second"}

    _LIVE._write_evidence_exclusive(path, first)
    with pytest.raises(FileExistsError):
        _LIVE._write_evidence_exclusive(path, second)

    assert json.loads(path.read_text(encoding="utf-8")) == first
    assert list(tmp_path.glob(".*.tmp")) == []


def test_live_evidence_validation_failure_publishes_nothing(tmp_path: Path) -> None:
    path = tmp_path / "invalid.json"

    def reject(_path: Path) -> None:
        raise ValueError("invalid evidence")

    with pytest.raises(ValueError, match="invalid evidence"):
        _LIVE._write_evidence_exclusive(
            path,
            {"run_id": "invalid"},
            validator=reject,
        )

    assert path.exists() is False
    assert list(tmp_path.glob(".*.tmp")) == []


def test_live_failed_output_path_is_distinct_from_success_target(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setattr(_LIVE.time, "strftime", lambda *_args: "20260713T010203Z")
    monkeypatch.setattr(_LIVE.os, "getpid", lambda: 42)
    requested = tmp_path / "focused.json"

    failed = _LIVE._failed_output_path(requested)

    assert failed == tmp_path / "focused.failed-20260713T010203Z-42.json"


@pytest.mark.parametrize(
    ("status", "expected"),
    [
        ({"state": "failed", "errorCode": "planning_failed"}, "planning_failed"),
        (
            {"state": "intervention_required", "errorCode": None},
            "intervention_required",
        ),
        (
            {
                "state": "stopped",
                "errorCode": "start_state_mismatch",
                "error": "Start state mismatch",
            },
            "start_state_mismatch",
        ),
        ({"state": "idle", "error": "reset failed"}, "reset failed"),
        (
            {"state": "running", "errorCode": "gripper_goal_aborted"},
            "gripper_goal_aborted",
        ),
        ({"state": "stopped", "errorCode": None, "error": None}, None),
        ({"state": "running", "errorCode": None}, None),
    ],
)
def test_live_cycle_failure_detection_covers_composer_stopped_faults(
    status: dict, expected: str | None
) -> None:
    assert _LIVE._cycle_failure_reason(status) == expected


def test_live_process_cpu_interval_reports_conservative_bounds() -> None:
    sampler = _LIVE._ProcessCpuSampler(1, 1.0)
    sampler._samples = [
        _LIVE._CpuSample(90, 1_000),
        _LIVE._CpuSample(110, 1_100),
        _LIVE._CpuSample(190, 1_400),
        _LIVE._CpuSample(210, 1_500),
    ]
    bounds = sampler.interval_bounds(100, 200)
    assert bounds == {
        "estimate_ns": 400,
        "lower_bound_ns": 300,
        "upper_bound_ns": 500,
        "uncertainty_ns": 200,
        "start_bracket_ns": 20,
        "end_bracket_ns": 20,
    }


def test_qualification_rma_view_uses_production_resolved_model_identity() -> None:
    corpus = json.loads(
        (_REPO_ROOT / "benchmarks/roboplan_05/corpus/corpus-v1.json").read_text(
            encoding="utf-8"
        )
    )
    view = _RUNNER._runtime_views(corpus, _REPO_ROOT)["ur10e_robotiq"]
    assert view.authority.model_name == "ur10e_robotiq_2f_140"


def _capture_scene_state() -> dict:
    return {
        "robot": {
            "robot_id": "ur10e",
            "base_frame": "base_link",
            "base_pose": {
                "position_m": [0.0, 0.0, 0.0],
                "orientation_rpy_deg": [0.0, 0.0, 90.0],
            },
            "joint_names": ["joint_a", "joint_b"],
            "joint_positions": {"joint_a": 0.1, "joint_b": -0.2},
            "state_source": "controller_state",
            "source_timestamp_ns": 100,
            "source_age_ns": 1_000_000,
        },
        "objects": [
            {
                "object_id": "drop_box",
                "shape": {
                    "type": "box",
                    "box_dimensions_m": [0.2, 0.3, 0.4],
                },
                "pose_in_universe": {
                    "position_m": [0.0, 1.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "attached": False,
                "attached_to": None,
            },
            {
                "object_id": "isaac_cheese_box",
                "shape": {
                    "type": "mesh",
                    "mesh_resource_uri": "examples/cheese.obj",
                    "mesh_scale": [1.0, 1.0, 1.0],
                },
                "pose_in_universe": {
                    "position_m": [0.2, 0.3, 0.4],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "attached": True,
                "attached_to": "grasp_frame",
            },
        ],
        "visual_state_diagnostics": {
            "served_from_visual_snapshot": True,
            "snapshot_stale": False,
            "snapshot_publisher_error": None,
            "objects_source": "visual_snapshot",
        },
    }


def _capture_program() -> dict:
    return {
        "name": "capture-test",
        "scene": [
            {
                "object_id": "isaac_cheese_box",
                "metadata": {
                    "support_contacts": [
                        {
                            "support_object_id": "drop_box",
                            "mode": "contact_release",
                            "max_start_penetration_m": 0.03,
                        }
                    ]
                },
            }
        ],
        "tree": {
            "id": "sequence",
            "name": "root",
            "children": [
                {"id": "move_manipulator_action", "name": "pick_retreat"},
                {
                    "id": "move_manipulator_action",
                    "name": "move_to_drop_approach",
                    "params": {
                        "relative_move": {
                            "target_reference": {
                                "type": "object",
                                "id": "drop_box",
                            },
                            "offset_reference": {
                                "type": "frame",
                                "id": "base_link",
                            },
                            "offset": {
                                "position_m": [0.0, 0.0, 0.4],
                                "orientation_rpy_deg": [0.0, 0.0, 0.0],
                            },
                        }
                    },
                },
            ],
        },
    }


def _capture_authority_snapshot() -> dict:
    return {
        "revision": {"revision_id": 7, "content_hash": "scene-seven"},
        "transaction_sequence": 11,
        "scene_admission_epoch": 3,
        "topology_epoch": 5,
        "policy_epoch": 2,
        "world_frame": "universe",
        "configured_collision_policy": {
            "policy_id": "default_free_space",
            "revision": {"revision_id": 1, "content_hash": "policy-one"},
            "allowed_static_static_pairs": [],
            "allowed_touch_link_pairs": [],
            "support_contacts": [],
            "contact_grasps": [],
            "require_full_diagnostics": False,
        },
        "robot_placements": [
            {
                "robot_id": "ur10e",
                "pose_in_world": {
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_xyzw": [
                        0.0,
                        0.0,
                        0.7071067811865475,
                        0.7071067811865476,
                    ],
                },
            }
        ],
        "objects": [
            {
                "object_id": "drop_box",
                "shape": {
                    "type": "box",
                    "box_dimensions_m": [0.2, 0.3, 0.4],
                },
                "pose_in_universe": {
                    "position_m": [0.0, 1.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "attached": False,
                "frame_id": "universe",
                "touch_links": [],
                "support_contacts": [],
            },
            {
                "object_id": "isaac_cheese_box",
                "shape": {
                    "type": "mesh",
                    "mesh_resource_uri": "examples/cheese.obj",
                    "mesh_scale": [1.0, 1.0, 1.0],
                },
                "pose_in_universe": {
                    "position_m": [0.2, 0.3, 0.4],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "attached": True,
                "frame_id": "grasp_frame",
                "touch_links": ["finger"],
                "support_contacts": [
                    {
                        "support_object_id": "drop_box",
                        "mode": "contact_release",
                        "max_start_penetration_m": 0.03,
                    }
                ],
            },
        ],
    }


def _capture_planning_record() -> dict:
    return {
        "sequence": 4,
        "request": {
            "robot_id": "ur10e",
            "group_id": "arm",
            "move_id": "drop_approach",
            "base_frame": "base_link",
            "tcp_frame": "grasp_frame",
            "start_state": {
                "joint_names": ["joint_a", "joint_b"],
                "positions_rad": [0.25, -0.5],
                "velocities_rad_s": [0.0, 0.0],
            },
            "motion": {
                "type": "pose_goal",
                "pose": {
                    "position_m": [1.0, 0.0, 0.4],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
            },
            "tolerances": {
                "joint_position_tol_rad": 0.05,
                "translation_tol_m": 0.005,
                "rotation_tol_rad": 0.05,
            },
            "speed_profile": {
                "velocity_scale": 0.2,
                "acceleration_scale": 1.0,
                "max_tcp_linear_speed_mps": 0.25,
            },
            "planning_timeout_s": 10.0,
            "planning_attempt_index": 0,
            "per_joint_dynamics_limits": {},
            "contact_policy_id": "",
            "action_span": None,
            "scene_binding": {
                "revision_id": 7,
                "content_hash": "scene-seven",
                "projection_lease_id": "lease-seven",
            },
        },
        "result": {"trajectory": None, "error": {"code": "planning_timeout"}},
        "metrics": {
            "rng_base_seed": 12345,
            "rng_seed": 12345,
            "shortcut_rng_seed_argument": -12346,
            "planning_attempt_index": 0,
        },
    }


def test_capture_payload_resolves_base_frame_goal_and_validates_semantic_hash(
    tmp_path: Path,
) -> None:
    program_path = tmp_path / "example.program.yaml"
    deployment_path = tmp_path / "example.deployment.yaml"
    program_path.write_text("name: test\n", encoding="utf-8")
    deployment_path.write_text("name: test\n", encoding="utf-8")
    payload = _CAPTURE._capture_payload(
        program=_capture_program(),
        visual_scene_state=_capture_scene_state(),
        authority_snapshot=_capture_authority_snapshot(),
        planning_record=_capture_planning_record(),
        run_id="run-new",
        planning_execution_id=17,
        program_path=program_path,
        deployment_path=deployment_path,
        repo_root=tmp_path,
        model_manifest={
            "authority": {
                "canonical_fingerprint": "a" * 64,
                "view_fingerprint": "b" * 64,
                "view_changed": True,
            }
        },
    )
    assert payload["resolved_motion"]["pose_goal"]["position_m"] == pytest.approx(
        [1.0, 0.0, 0.4], abs=1e-12
    )
    assert payload["start_state"] == {
        "joint_names": ["joint_a", "joint_b"],
        "positions_rad": [0.25, -0.5],
        "velocities_rad_s": [0.0, 0.0],
        "state_source": "exact_plan_request",
        "provenance": {
            "execution_state_source_mode": "commanded_sim",
            "capture_method": "planner_qualification_observer",
            "predecessor_move_id": "pick_retreat",
        },
    }
    assert payload["plan_request"]["speed_profile"] == {
        "velocity_scale": 0.2,
        "acceleration_scale": 1.0,
        "max_tcp_linear_speed_mps": 0.25,
    }
    assert payload["scene"]["authority"]["revision"] == {
        "revision_id": 7,
        "content_hash": "scene-seven",
    }
    cheese = next(
        item
        for item in payload["scene"]["objects"]
        if item["object_id"] == "isaac_cheese_box"
    )
    assert cheese["support_contacts"] == [
        {
            "support_object_id": "drop_box",
            "mode": "contact_release",
            "max_start_penetration_m": 0.03,
        }
    ]
    path = tmp_path / "capture.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    summary = _MODULE.validate_capture(
        path,
        expected_capture_id="isaac-light-drop-approach-v1",
        expected_joint_names=["joint_a", "joint_b"],
        required_attachment="isaac_cheese_box",
    )
    assert summary["objects"] == 2
    nonzero_velocity = json.loads(json.dumps(payload))
    nonzero_velocity["start_state"]["velocities_rad_s"][0] = 0.1
    nonzero_velocity["plan_request"]["start_state"]["velocities_rad_s"][0] = 0.1
    unsigned = dict(nonzero_velocity)
    unsigned.pop("semantic_sha256")
    nonzero_velocity["semantic_sha256"] = hashlib.sha256(
        json.dumps(unsigned, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    path.write_text(json.dumps(nonzero_velocity), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="velocities must.*zero"):
        _MODULE.validate_capture(path)
    tiny_nonzero_velocity = json.loads(json.dumps(payload))
    tiny_nonzero_velocity["start_state"]["velocities_rad_s"][0] = 1e-15
    tiny_nonzero_velocity["plan_request"]["start_state"]["velocities_rad_s"][
        0
    ] = 1e-15
    unsigned = dict(tiny_nonzero_velocity)
    unsigned.pop("semantic_sha256")
    tiny_nonzero_velocity["semantic_sha256"] = hashlib.sha256(
        json.dumps(unsigned, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    path.write_text(json.dumps(tiny_nonzero_velocity), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="velocities must.*zero"):
        _MODULE.validate_capture(path)
    path.write_text(json.dumps(payload), encoding="utf-8")
    payload["source"]["captured_at_wall_ns"] += 1
    path.write_text(json.dumps(payload), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="semantic_sha256"):
        _MODULE.validate_capture(path)


def test_capture_wait_uses_active_drop_planning_window_and_fresh_snapshot(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    responses = iter(
        [
            {
                "state": "planning",
                "active_move_id": "drop_approach",
                "execution_id": 17,
                "joint_positions_rad": [0.25, -0.5],
            },
            {"runId": "run-new"},
            {
                **_capture_scene_state(),
            },
            _capture_authority_snapshot(),
            {
                "state": "planning",
                "active_move_id": "drop_approach",
                "execution_id": 17,
                "joint_positions_rad": [0.25, -0.5],
            },
        ]
    )
    monkeypatch.setattr(_CAPTURE, "_request_json", lambda *_args, **_kwargs: next(responses))
    (
        run_id,
        execution_id,
        scene,
        planning_snapshot,
        authority_snapshot,
    ) = _CAPTURE._wait_for_capture_point(
        "http://composer",
        1.0,
        previous_run_id="run-old",
        previous_execution_id=16,
        max_source_age_ns=250_000_000,
    )
    assert run_id == "run-new"
    assert execution_id == 17
    assert scene["robot"]["state_source"] == "controller_state"
    assert planning_snapshot["joint_positions_rad"] == [0.25, -0.5]
    assert authority_snapshot["revision"]["revision_id"] == 7


def test_capture_wait_fails_promptly_on_composer_fault(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    responses = iter(
        [
            {"state": "idle", "execution_id": 0},
            {
                "state": "stopped",
                "errorCode": "gripper_goal_aborted",
                "error": "Gripper action aborted",
            },
        ]
    )
    monkeypatch.setattr(
        _CAPTURE, "_request_json", lambda *_args, **_kwargs: next(responses)
    )

    with pytest.raises(RuntimeError, match="gripper_goal_aborted"):
        _CAPTURE._wait_for_capture_point(
            "http://composer",
            1.0,
            previous_run_id="run-old",
            previous_execution_id=16,
            max_source_age_ns=250_000_000,
        )


def test_capture_waits_for_complete_successful_drop_plan_record(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    responses = iter(
        [
            {"records": [], "status": {"in_flight": 1}},
            {"state": "running"},
            {
                "records": [
                    {
                        "request": {
                            "move_id": "drop_approach",
                            "action_span": {
                                "program_run_id": "run-prior",
                                "node_attempt_id": "attempt-prior",
                            },
                        },
                        "result": {"trajectory": {"waypoints": [{}]}},
                        "metrics": {
                            "measurement_complete": True,
                            "succeeded": True,
                            "shortcut_rng_seed_argument": -12346,
                        },
                    },
                    {
                        "request": {
                            "move_id": "drop_approach",
                            "action_span": {
                                "program_run_id": "run-new",
                                "node_attempt_id": "attempt-new",
                            },
                        },
                        "result": {"trajectory": {"waypoints": [{}]}},
                        "metrics": {
                            "measurement_complete": True,
                            "succeeded": True,
                            "shortcut_rng_seed_argument": -12346,
                        },
                    }
                ],
                "status": {"in_flight": 0},
            },
        ]
    )
    monkeypatch.setattr(
        _CAPTURE, "_request_json", lambda *_args, **_kwargs: next(responses)
    )
    monkeypatch.setattr(_CAPTURE.time, "sleep", lambda _duration: None)

    records = _CAPTURE._wait_for_completed_drop_plan_record(
        "http://composer",
        generation=3,
        timeout_s=1.0,
        expected_program_run_id="run-new",
        expected_node_attempt_id="attempt-new",
    )

    assert len(records) == 2
    assert records[1]["metrics"]["shortcut_rng_seed_argument"] == -12346


def test_capture_correlates_planner_attempt_by_run_and_execution(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        _CAPTURE,
        "_request_json",
        lambda *_args, **_kwargs: {
            "events": [
                {
                    "type": "plan_started",
                    "execution_id": 7,
                    "program_run_id": "run-old",
                    "details": {"node_attempt_id": "attempt-old"},
                },
                {
                    "type": "plan_started",
                    "execution_id": 7,
                    "program_run_id": "run-new",
                    "details": {"node_attempt_id": "attempt-new"},
                },
            ]
        },
    )

    assert _CAPTURE._planning_attempt_id_for_execution(
        "http://composer",
        program_run_id="run-new",
        execution_id=7,
    ) == "attempt-new"


def test_capture_simulation_reset_and_play_use_deployment_control_contract(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[tuple[str, str, dict, float]] = []

    def request(
        _base_url: str,
        path: str,
        *,
        method: str,
        payload: dict,
        timeout_s: float,
    ) -> dict:
        calls.append((path, method, payload, timeout_s))
        if path == "/api/simulation/state":
            return {"success": True, "state": payload["state"]}
        return {"success": True}

    monkeypatch.setattr(_CAPTURE, "_request_json", request)
    _CAPTURE.reset_and_start_simulation("http://composer", 42.0)
    assert calls == [
        ("/api/simulation/reset", "POST", {}, 42.0),
        (
            "/api/simulation/state",
            "POST",
            {"state": "playing", "reason": "rp5_qualification_start"},
            42.0,
        ),
    ]


def test_capture_requires_loaded_saved_artifacts_to_match(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    program_path = (tmp_path / "program.yaml").resolve()
    deployment_path = (tmp_path / "deployment.yaml").resolve()
    program_path.write_text("program-v1\n", encoding="utf-8")
    deployment_path.write_text("deployment-v1\n", encoding="utf-8")
    responses = {
        "/api/program": {
            "name": "isaac-light",
            "sourcePath": str(program_path),
        },
        "/api/program/tree": {"tree": {"id": "sequence"}},
        "/api/program/saved/tree": {"tree": {"id": "sequence"}},
        "/api/deployment/full": {
            "name": "isaac-light-ros",
            "sourcePath": str(deployment_path),
        },
        "/api/deployment/saved/full": {
            "name": "isaac-light-ros",
            "sourcePath": str(deployment_path),
        },
    }
    monkeypatch.setattr(
        _CAPTURE,
        "_request_json",
        lambda _base_url, path, **_kwargs: responses[path],
    )

    provenance = _CAPTURE.require_loaded_artifacts_match(
        "http://composer",
        program_path=program_path,
        deployment_path=deployment_path,
        expected_program_name="isaac-light",
    )
    assert provenance == {
        "program": {
            "name": "isaac-light",
            "path": str(program_path),
            "sha256": hashlib.sha256(program_path.read_bytes()).hexdigest(),
        },
        "deployment": {
            "name": "isaac-light-ros",
            "path": str(deployment_path),
            "sha256": hashlib.sha256(deployment_path.read_bytes()).hexdigest(),
        },
    }

    responses["/api/program/tree"] = {"tree": {"id": "modified"}}
    with pytest.raises(RuntimeError, match="unsaved or runtime-overridden"):
        _CAPTURE.require_loaded_artifacts_match(
            "http://composer",
            program_path=program_path,
            deployment_path=deployment_path,
            expected_program_name="isaac-light",
        )


def test_captured_replay_rejects_changed_source_or_model_view(tmp_path: Path) -> None:
    program = tmp_path / "program.yaml"
    deployment = tmp_path / "deployment.yaml"
    program.write_text("program-v1", encoding="utf-8")
    deployment.write_text("deployment-v1", encoding="utf-8")
    capture = {
        "source": {
            "program": "program.yaml",
            "program_sha256": hashlib.sha256(program.read_bytes()).hexdigest(),
            "deployment": "deployment.yaml",
            "deployment_sha256": hashlib.sha256(
                deployment.read_bytes()
            ).hexdigest(),
        },
        "robot_model_authority": {
            "canonical_fingerprint": "a" * 64,
            "view_fingerprint": "b" * 64,
        },
    }
    capture_path = tmp_path / "capture.json"
    capture_path.write_text(json.dumps(capture), encoding="utf-8")
    view = SimpleNamespace(
        authority=SimpleNamespace(canonical_fingerprint="a" * 64),
        import_view=SimpleNamespace(view_fingerprint="b" * 64),
    )
    assert _RUNNER._verify_capture_provenance(
        capture_path=capture_path,
        repo_root=tmp_path,
        view=view,
    ) == capture
    program.write_text("program-v2", encoding="utf-8")
    with pytest.raises(RuntimeError, match="changed after capture"):
        _RUNNER._verify_capture_provenance(
            capture_path=capture_path,
            repo_root=tmp_path,
            view=view,
        )
    program.write_text("program-v1", encoding="utf-8")
    view.import_view.view_fingerprint = "c" * 64
    with pytest.raises(RuntimeError, match="view fingerprint"):
        _RUNNER._verify_capture_provenance(
            capture_path=capture_path,
            repo_root=tmp_path,
            view=view,
        )
