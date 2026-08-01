"""Regression tests for the version-neutral RoboPlan transition artifacts."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
from types import SimpleNamespace

from jsonschema import Draft202012Validator
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
_LIVE_SCRIPT = (
    _REPO_ROOT / "scripts/qualification/run_roboplan_05_live_baseline.py"
)
_LIVE_SPEC = importlib.util.spec_from_file_location(
    "run_roboplan_05_live_baseline", _LIVE_SCRIPT
)
assert _LIVE_SPEC is not None and _LIVE_SPEC.loader is not None
_LIVE = importlib.util.module_from_spec(_LIVE_SPEC)
_LIVE_SPEC.loader.exec_module(_LIVE)


def test_roboplan_05_inventory_binds_capabilities_workarounds_and_patches() -> None:
    capabilities, workarounds = _MODULE._version_inventory(  # noqa: SLF001
        _REPO_ROOT, _MODULE.ROBOPLAN_05_VERSION
    )
    capability_by_name = {
        item["capability"]: item for item in capabilities["capabilities"]
    }
    workaround_by_id = {
        item["id"]: item for item in workarounds["workarounds"]
    }
    assert capability_by_name["planning.motion.cartesian_path"]["status"] == (
        "supported"
    )
    assert capability_by_name["planning.motion.named_target"]["status"] == (
        "unsupported"
    )
    assert workaround_by_id["roboplan_04_negative_shortcut_seed_encoding"][
        "status"
    ] == "retired"
    assert workaround_by_id["implicit_scene_copy_for_revision_retention"][
        "status"
    ] == "retired"

    patch_manifest = _MODULE._checked_patch_manifest(_REPO_ROOT)  # noqa: SLF001
    assert len(patch_manifest["patches"]) == 7
    assert all(len(item["sha256"]) == 64 for item in patch_manifest["patches"])


def test_patch_manifest_rejects_unlisted_patch(tmp_path: Path) -> None:
    root = tmp_path / "third_party/roboplan"
    patches = root / "patches"
    patches.mkdir(parents=True)
    listed = patches / "0001-listed.patch"
    listed.write_text("listed\n", encoding="utf-8")
    (patches / "0002-unlisted.patch").write_text("unlisted\n", encoding="utf-8")
    digest = hashlib.sha256(listed.read_bytes()).hexdigest()
    (root / "patches.sha256").write_text(
        f"{digest}  patches/{listed.name}\n", encoding="utf-8"
    )

    with pytest.raises(_MODULE.ValidationError, match="unlisted"):
        _MODULE._checked_patch_manifest(tmp_path)  # noqa: SLF001


def test_verified_checkout_rejects_dirty_source(tmp_path: Path) -> None:
    docker_root = tmp_path / ".docker"
    checkout = docker_root / "roboplan"
    checkout.mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(checkout)], check=True)
    source = checkout / "source.txt"
    source.write_text("reviewed\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(checkout), "add", "source.txt"], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(checkout),
            "-c",
            "user.name=ManyForge Test",
            "-c",
            "user.email=manyforge-test@example.invalid",
            "commit",
            "-q",
            "-m",
            "fixture",
        ],
        check=True,
    )
    commit = subprocess.check_output(
        ["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True
    ).strip()
    (docker_root / ".manyforge-roboplan-ref").write_text(
        f"{commit}\n", encoding="utf-8"
    )
    (docker_root / ".manyforge-roboplan-sha").write_text(
        f"{commit}\n", encoding="utf-8"
    )

    assert _MODULE._verified_roboplan_checkout(tmp_path) == (  # noqa: SLF001
        checkout,
        commit,
        commit,
    )
    source.write_text("modified\n", encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="dirty"):
        _MODULE._verified_roboplan_checkout(tmp_path)  # noqa: SLF001


def test_dirty_worktree_fingerprint_changes_with_source_content(
    tmp_path: Path,
) -> None:
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    source = tmp_path / "source.txt"
    source.write_text("committed\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(tmp_path), "add", "source.txt"], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(tmp_path),
            "-c",
            "user.name=ManyForge Test",
            "-c",
            "user.email=manyforge-test@example.invalid",
            "commit",
            "-q",
            "-m",
            "fixture",
        ],
        check=True,
    )

    source.write_text("dirty-one\n", encoding="utf-8")
    first = _MODULE._manyforge_worktree_sha256(tmp_path)  # noqa: SLF001
    source.write_text("dirty-two\n", encoding="utf-8")
    second = _MODULE._manyforge_worktree_sha256(tmp_path)  # noqa: SLF001
    assert first != second

    with pytest.raises(RuntimeError, match="source identity changed"):
        _MODULE.require_source_inventory_stable(
            {"source": {"manyforge_dirty": True, "fingerprint": first}},
            {"source": {"manyforge_dirty": True, "fingerprint": second}},
        )


def test_worktree_delta_distinguishes_regular_file_from_symlink(
    tmp_path: Path,
) -> None:
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    tracked = tmp_path / "tracked.txt"
    tracked.write_text("committed\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(tmp_path), "add", "tracked.txt"], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(tmp_path),
            "-c",
            "user.name=ManyForge Test",
            "-c",
            "user.email=manyforge-test@example.invalid",
            "commit",
            "-q",
            "-m",
            "fixture",
        ],
        check=True,
    )

    entry = tmp_path / "entry"
    entry.write_text("target", encoding="utf-8")
    entry.chmod(0o777)
    regular = _MODULE._manyforge_worktree_delta_sha256(tmp_path)  # noqa: SLF001
    entry.unlink()
    entry.symlink_to("target")
    symlink = _MODULE._manyforge_worktree_delta_sha256(tmp_path)  # noqa: SLF001

    assert regular != symlink


def test_source_fingerprint_is_deterministic_and_binds_clean_head(
    tmp_path: Path,
) -> None:
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    source = tmp_path / "source.txt"
    source.write_text("committed\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(tmp_path), "add", "source.txt"], check=True)

    commit = [
        "git",
        "-C",
        str(tmp_path),
        "-c",
        "user.name=ManyForge Test",
        "-c",
        "user.email=manyforge-test@example.invalid",
        "commit",
        "-q",
    ]
    subprocess.run([*commit, "-m", "fixture"], check=True)
    first = _MODULE._manyforge_source_identity(tmp_path)  # noqa: SLF001
    assert first == _MODULE._manyforge_source_identity(tmp_path)  # noqa: SLF001
    assert first["dirty"] is False
    assert first["delta_sha256"] == _MODULE._manyforge_worktree_sha256(  # noqa: SLF001
        tmp_path
    )

    # An empty commit deliberately retains the same tree and clean delta. The
    # composite must still change because HEAD itself is part of the identity.
    subprocess.run([*commit, "--allow-empty", "-m", "same tree, new head"], check=True)
    second = _MODULE._manyforge_source_identity(tmp_path)  # noqa: SLF001
    assert second["head_commit"] != first["head_commit"]
    assert second["head_tree"] == first["head_tree"]
    assert second["delta_sha256"] == first["delta_sha256"]
    assert second["fingerprint_sha256"] != first["fingerprint_sha256"]


def test_source_fingerprint_binds_dirty_delta(tmp_path: Path) -> None:
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    source = tmp_path / "source.txt"
    source.write_text("committed\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(tmp_path), "add", "source.txt"], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(tmp_path),
            "-c",
            "user.name=ManyForge Test",
            "-c",
            "user.email=manyforge-test@example.invalid",
            "commit",
            "-q",
            "-m",
            "fixture",
        ],
        check=True,
    )

    source.write_text("dirty-one\n", encoding="utf-8")
    first = _MODULE._manyforge_source_identity(tmp_path)  # noqa: SLF001
    source.write_text("dirty-two\n", encoding="utf-8")
    second = _MODULE._manyforge_source_identity(tmp_path)  # noqa: SLF001
    assert first["dirty"] is True
    assert second["dirty"] is True
    assert first["head_commit"] == second["head_commit"]
    assert first["head_tree"] == second["head_tree"]
    assert first["delta_sha256"] != second["delta_sha256"]
    assert first["fingerprint_sha256"] != second["fingerprint_sha256"]


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
    captured = next(
        fixture
        for fixture in value["planning_fixtures"]
        if fixture["id"] == "ur10e_drop_approach_capture"
    )
    assert captured["capture_source_policy"] == "frozen_rp5_0_manifest"


def test_checked_in_capture_matches_schema_semantics_and_frozen_manifest() -> None:
    capture_path = (
        _REPO_ROOT
        / "benchmarks/roboplan_05/corpus/captures/isaac-light-drop-approach-v1.json"
    )
    schema = json.loads(
        (
            _REPO_ROOT / "benchmarks/roboplan_05/schema/capture-v1.schema.json"
        ).read_text(encoding="utf-8")
    )
    capture = json.loads(capture_path.read_text(encoding="utf-8"))
    Draft202012Validator(schema).validate(capture)
    assert _MODULE.validate_capture(capture_path)["capture_id"] == (
        "isaac-light-drop-approach-v1"
    )

    manifest = json.loads(
        (
            _REPO_ROOT
            / "benchmarks/roboplan_05/baselines/roboplan-0.4.0-manifest.json"
        ).read_text(encoding="utf-8")
    )
    assert capture["semantic_sha256"] == manifest["evidence"][
        "captured_authority_fixture_semantic_sha256"
    ]


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


@pytest.mark.parametrize(
    "program_name",
    [
        "isaac_manipulator_scene_light.program.yaml",
        "isaac_manipulator_scene_light_collision.program.yaml",
    ],
)
def test_isaac_light_drop_release_keeps_attached_object_clear_of_klt(
    program_name: str,
) -> None:
    program = yaml.safe_load(
        (_REPO_ROOT / "examples" / program_name).read_text(encoding="utf-8")
    )

    def descendants(node: dict[str, object]) -> list[dict[str, object]]:
        children = list(node.get("children") or [])
        return [node, *[nested for child in children for nested in descendants(child)]]

    nodes = {str(node.get("name")): node for node in descendants(program["tree"])}
    approach_offset = nodes["move_to_drop_approach"]["params"]["pose_goal"][
        "position_m"
    ]
    release_offset = nodes["move_to_drop"]["params"]["pose_goal"]["position_m"]

    assert approach_offset == [0, 0, 0.4]
    assert release_offset == [0, 0, 0.35]


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


def test_corpus_rejects_unknown_capture_source_policy(tmp_path: Path) -> None:
    source = _REPO_ROOT / "benchmarks/roboplan_05/corpus/corpus-v1.json"
    corpus = json.loads(source.read_text(encoding="utf-8"))
    captured = next(
        fixture
        for fixture in corpus["planning_fixtures"]
        if fixture.get("capture_file")
    )
    captured["capture_source_policy"] = "trust_stale_source"
    path = tmp_path / "corpus.json"
    path.write_text(json.dumps(corpus), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="unsupported"):
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


def test_evidence_validator_rejects_unknown_record_type() -> None:
    with pytest.raises(_MODULE.ValidationError, match="record_type is not supported"):
        _MODULE._validate_rp5_record(  # noqa: SLF001
            {
                "record_type": "future_unvalidated_measurement",
                "fixture_id": "unknown",
                "passed": True,
            },
            "record",
            formal=True,
        )


@pytest.mark.parametrize(
    ("record", "message"),
    [
        (
            {
                "record_type": "collision_revision",
                "fixture_id": "revision",
                "passed": True,
                "latency_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
                "details": {
                    "allocations": 4,
                    "allocations_per_query": 2.0,
                    "allocation_bound_per_query": 1,
                    "wcet_budget_ns": 10,
                },
            },
            "exceeds its bound",
        ),
        (
            {
                "record_type": "trajectory_validation",
                "fixture_id": "validation",
                "passed": True,
                "sample_count": 9,
                "latency_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
                "details": {
                    "collision_validation": {"enabled": False},
                    "noncollision_validation": {"enabled": False},
                },
            },
            "enabled validation surface",
        ),
        (
            {
                "record_type": "rma_contract",
                "fixture_id": "model",
                "passed": True,
                "details": {
                    "canonical_fingerprint": "a" * 64,
                    "view_fingerprint": "b" * 64,
                    "view_changed": False,
                    "mapping_bijective": False,
                    "public_planning_joint_names_preserved": True,
                    "renamed_joint_count": 0,
                },
            },
            "passed without preserving",
        ),
    ],
)
def test_evidence_validator_rejects_malformed_auxiliary_records(
    record: dict, message: str
) -> None:
    with pytest.raises(_MODULE.ValidationError, match=message):
        _MODULE._validate_rp5_record(  # noqa: SLF001
            record,
            "record",
            formal=True,
            roboplan_version=_MODULE.ROBOPLAN_05_VERSION,
        )


def test_frozen_roboplan_04_offline_evidence_remains_valid() -> None:
    evidence = _REPO_ROOT / "build/qualification/rp5-0-offline-2952090.json"
    if not evidence.is_file():
        pytest.skip("local frozen RP5-0 raw evidence is not installed")
    assert _MODULE.validate_evidence(evidence)["records"] == 70


def _promote_to_formal_roboplan_05(evidence: dict) -> dict:
    evidence["phase"] = "rp5_3"
    evidence["source"].update(
        {
            "roboplan_version": _MODULE.ROBOPLAN_05_VERSION,
            "roboplan_commit": _MODULE.ROBOPLAN_05_PINNED_COMMIT,
            "roboplan_ref": _MODULE.ROBOPLAN_05_PINNED_COMMIT,
            "pinocchio_version": _MODULE.ROBOPLAN_05_PINOCCHIO_VERSION,
            "coal_version": _MODULE.ROBOPLAN_05_COAL_VERSION,
            "roboplan_package_versions": dict(
                _MODULE.ROBOPLAN_05_PACKAGE_VERSIONS
            ),
            "roboplan_patch_manifest_sha256": (
                _MODULE.ROBOPLAN_05_PATCH_MANIFEST_SHA256
            ),
            "roboplan_patches": [
                {"path": path, "sha256": digest}
                for path, digest in _MODULE.ROBOPLAN_05_PATCH_DIGESTS.items()
            ],
        }
    )
    return evidence


def test_formal_roboplan_05_evidence_requires_patch_provenance(
    tmp_path: Path,
) -> None:
    evidence = _promote_to_formal_roboplan_05(_formal_live_evidence())
    del evidence["source"]["roboplan_patch_manifest_sha256"]
    del evidence["source"]["roboplan_patches"]
    path = tmp_path / "missing-patch-provenance.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")

    with pytest.raises(_MODULE.ValidationError, match="patch_manifest"):
        _MODULE.validate_evidence(path)


def test_formal_rp5_3_evidence_rejects_superseded_roboplan_050(
    tmp_path: Path,
) -> None:
    evidence = _formal_live_evidence()
    evidence["phase"] = "rp5_3"
    evidence["source"]["roboplan_version"] = "0.5.0"
    path = tmp_path / "superseded-roboplan-050.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")

    with pytest.raises(_MODULE.ValidationError, match="requires RoboPlan 0.5.1"):
        _MODULE.validate_evidence(path)


def test_formal_roboplan_05_evidence_is_bound_to_reviewed_patch_set(
    tmp_path: Path,
) -> None:
    evidence = _promote_to_formal_roboplan_05(_formal_live_evidence())
    source = evidence["source"]
    path = tmp_path / "reviewed-patches.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")

    assert _MODULE.validate_evidence(path)["records"] == 1
    source["roboplan_patches"][0]["sha256"] = "f" * 64
    path.write_text(json.dumps(evidence), encoding="utf-8")
    with pytest.raises(_MODULE.ValidationError, match="reviewed manifest"):
        _MODULE.validate_evidence(path)


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("pinocchio_version", "4.0.0", "Pinocchio 4.1.0"),
        ("coal_version", "3.0.2", "Coal 3.0.3"),
    ],
)
def test_formal_roboplan_05_evidence_requires_exact_native_dependencies(
    tmp_path: Path, field: str, value: str, message: str
) -> None:
    evidence = _promote_to_formal_roboplan_05(_formal_live_evidence())
    evidence["source"][field] = value
    path = tmp_path / f"wrong-{field}.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")

    with pytest.raises(_MODULE.ValidationError, match=message):
        _MODULE.validate_evidence(path)


@pytest.mark.parametrize("mutation", ["missing", "wrong"])
def test_formal_roboplan_05_evidence_requires_exact_package_generation(
    tmp_path: Path, mutation: str
) -> None:
    evidence = _promote_to_formal_roboplan_05(_formal_live_evidence())
    versions = evidence["source"]["roboplan_package_versions"]
    if mutation == "missing":
        del versions["roboplan_rrt"]
    else:
        versions["roboplan_rrt"] = "0.5.0"
    path = tmp_path / f"{mutation}-package-version.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")

    with pytest.raises(_MODULE.ValidationError, match="package-version inventory"):
        _MODULE.validate_evidence(path)


@pytest.mark.parametrize(
    ("policy", "mode", "rrt_star", "fast_return"),
    [
        ("compatibility", "primary", False, True),
        ("compatibility", "collision_probe_attribution", False, True),
        ("rrt_star", "capability_opt_in", True, False),
        ("budget_spending", "capability_opt_in", False, False),
    ],
)
def test_planning_evidence_keeps_opt_in_policies_separate(
    policy: str,
    mode: str,
    rrt_star: bool,
    fast_return: bool,
) -> None:
    probe = mode == "collision_probe_attribution"
    record = {
        "record_type": "planning_pipeline",
        "fixture_id": "planner-policy",
        "passed": True,
        "sample_count": 50,
        "measurement_mode": mode,
        "planner_policy": policy,
        "planner_options": {
            "rrt_star": rrt_star,
            "fast_return": fast_return,
        },
        "collision_probe_available": probe,
        "total_collision_query_count": 1 if probe else 0,
        "stages": (
            {
                "rrt": {
                    "collision_query_count": {
                        "p50": 1,
                        "p95": 1,
                        "p99": 1,
                        "max": 1,
                    }
                }
            }
            if probe
            else {}
        ),
        "invariants_passed": True,
        "outcomes_passed": True,
        "deterministic_trajectory": True,
        "distinct_trajectory_hashes": 1,
        "latency_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
        "cpu_time_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
        "allocation_count": {"p50": 0, "p95": 0, "p99": 0, "max": 0},
    }
    _MODULE._validate_rp5_record(  # noqa: SLF001
        record,
        "record",
        formal=True,
        roboplan_version=_MODULE.ROBOPLAN_05_VERSION,
    )


def test_roboplan_05_attribution_rejects_scene_only_collision_probe() -> None:
    record = {
        "record_type": "planning_pipeline",
        "fixture_id": "scene-only-probe",
        "passed": True,
        "sample_count": 50,
        "measurement_mode": "collision_probe_attribution",
        "planner_policy": "compatibility",
        "planner_options": {"rrt_star": False, "fast_return": True},
        "collision_probe_available": True,
        "total_collision_query_count": 50,
        "stages": {
            "start_collision_query": {
                "collision_query_count": {
                    "p50": 1,
                    "p95": 1,
                    "p99": 1,
                    "max": 1,
                }
            },
            "rrt": {
                "collision_query_count": {
                    "p50": 0,
                    "p95": 0,
                    "p99": 0,
                    "max": 0,
                }
            },
        },
        "invariants_passed": True,
        "outcomes_passed": True,
        "deterministic_trajectory": True,
        "distinct_trajectory_hashes": 1,
        "latency_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
        "cpu_time_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
        "allocation_count": {"p50": 0, "p95": 0, "p99": 0, "max": 0},
    }

    with pytest.raises(_MODULE.ValidationError, match="CollisionContext"):
        _MODULE._validate_rp5_record(  # noqa: SLF001
            record,
            "record",
            formal=True,
            roboplan_version=_MODULE.ROBOPLAN_05_VERSION,
        )


@pytest.mark.parametrize(
    ("policy", "mode", "rrt_star", "fast_return"),
    [
        ("compatibility", "capability_opt_in", False, True),
        ("rrt_star", "primary", True, False),
        ("rrt_star", "capability_opt_in", False, False),
        ("budget_spending", "capability_opt_in", True, False),
    ],
)
def test_planning_evidence_rejects_mislabeled_policy_rows(
    policy: str,
    mode: str,
    rrt_star: bool,
    fast_return: bool,
) -> None:
    record = {
        "record_type": "planning_pipeline",
        "fixture_id": "planner-policy",
        "passed": True,
        "sample_count": 50,
        "measurement_mode": mode,
        "planner_policy": policy,
        "planner_options": {
            "rrt_star": rrt_star,
            "fast_return": fast_return,
        },
        "collision_probe_available": False,
        "total_collision_query_count": 0,
        "invariants_passed": True,
        "outcomes_passed": True,
        "deterministic_trajectory": True,
        "distinct_trajectory_hashes": 1,
        "latency_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
        "cpu_time_ns": {"p50": 1, "p95": 2, "p99": 3, "max": 4},
        "allocation_count": {"p50": 0, "p95": 0, "p99": 0, "max": 0},
    }
    with pytest.raises(_MODULE.ValidationError):
        _MODULE._validate_rp5_record(  # noqa: SLF001
            record,
            "record",
            formal=True,
            roboplan_version=_MODULE.ROBOPLAN_05_VERSION,
        )


@pytest.mark.parametrize("phase", ["rp5_0", "rp5_3", "rp5_7"])
def test_evidence_validator_rejects_undersampled_clean_source_evidence(
    tmp_path: Path, phase: str,
) -> None:
    evidence = {
        "schema_version": "manyforge.roboplan_transition_evidence.v1",
        "run_id": "formal-run",
        "phase": phase,
        "source": {
            "manyforge_dirty": False,
            "manyforge_commit": "a" * 40,
            "roboplan_source_clean": True,
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
    if phase != "rp5_0":
        _promote_to_formal_roboplan_05(evidence)
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
            "roboplan_source_clean": True,
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


def _freeze_formal_evidence_source(evidence: dict) -> dict:
    source = evidence["source"]
    delta_sha256 = "e" * 64
    identity = {
        "schema_version": _MODULE.MANYFORGE_SOURCE_IDENTITY_SCHEMA,
        "head_commit": source["manyforge_commit"],
        "head_tree": "f" * 40,
        "dirty": True,
        "delta_sha256": delta_sha256,
    }
    identity["fingerprint_sha256"] = (
        _MODULE._manyforge_source_fingerprint_sha256(  # noqa: SLF001
            head_commit=identity["head_commit"],
            head_tree=identity["head_tree"],
            dirty=identity["dirty"],
            delta_sha256=identity["delta_sha256"],
        )
    )
    source.update(
        {
            "manyforge_dirty": True,
            "manyforge_worktree_sha256": delta_sha256,
            "manyforge_worktree_delta_sha256": delta_sha256,
            "manyforge_source_identity": identity,
            "manyforge_source_fingerprint_sha256": identity[
                "fingerprint_sha256"
            ],
            "manyforge_source_identity_mode": (
                _MODULE.MANYFORGE_FROZEN_SOURCE_IDENTITY_MODE
            ),
            "manyforge_expected_source_fingerprint_sha256": identity[
                "fingerprint_sha256"
            ],
        }
    )
    return evidence


def test_frozen_dirty_source_identity_is_formal(tmp_path: Path) -> None:
    evidence = _freeze_formal_evidence_source(
        _promote_to_formal_roboplan_05(_formal_live_evidence())
    )
    path = tmp_path / "frozen-dirty-formal.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")

    assert _MODULE.validate_evidence(path)["records"] == 1


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("missing_identity", "manyforge_source_identity is required"),
        ("missing_expected", "expected_source_fingerprint_sha256 is required"),
        ("wrong_expected", "does not match the observed source fingerprint"),
        ("malformed_tree", "head_tree must be a full Git SHA"),
        ("wrong_internal_fingerprint", "fingerprint_sha256 is inconsistent"),
        ("wrong_top_delta", "worktree_delta_sha256 is inconsistent"),
        ("wrong_top_commit", "manyforge_commit is inconsistent"),
        ("clean_declaration", "manyforge_dirty is inconsistent"),
    ],
)
def test_frozen_dirty_source_rejects_incomplete_or_inconsistent_identity(
    tmp_path: Path, mutation: str, message: str
) -> None:
    evidence = _freeze_formal_evidence_source(
        _promote_to_formal_roboplan_05(_formal_live_evidence())
    )
    source = evidence["source"]
    if mutation == "missing_identity":
        del source["manyforge_source_identity"]
    elif mutation == "missing_expected":
        del source["manyforge_expected_source_fingerprint_sha256"]
    elif mutation == "wrong_expected":
        source["manyforge_expected_source_fingerprint_sha256"] = "0" * 64
    elif mutation == "malformed_tree":
        source["manyforge_source_identity"]["head_tree"] = "short"
    elif mutation == "wrong_internal_fingerprint":
        source["manyforge_source_identity"]["fingerprint_sha256"] = "0" * 64
    elif mutation == "wrong_top_delta":
        source["manyforge_worktree_delta_sha256"] = "0" * 64
    elif mutation == "wrong_top_commit":
        source["manyforge_commit"] = "0" * 40
    elif mutation == "clean_declaration":
        source["manyforge_dirty"] = False
    else:  # pragma: no cover - parametrization is exhaustive
        raise AssertionError(mutation)
    path = tmp_path / f"frozen-{mutation}.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")

    with pytest.raises(_MODULE.ValidationError, match=message):
        _MODULE.validate_evidence(path)


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("provenance", "pinocchio_version"),
        ("inventory", "complete inventory"),
        ("artifact", "source.program must be an object"),
        ("sample_size", "at least 30"),
    ],
)
def test_frozen_dirty_source_gets_all_formal_evidence_rules(
    tmp_path: Path, mutation: str, message: str
) -> None:
    evidence = _freeze_formal_evidence_source(
        _promote_to_formal_roboplan_05(_formal_live_evidence())
    )
    if mutation == "provenance":
        del evidence["source"]["pinocchio_version"]
    elif mutation == "inventory":
        del evidence["inventory"]
    elif mutation == "artifact":
        del evidence["source"]["program"]
    elif mutation == "sample_size":
        evidence["records"][0]["sample_count"] = 29
    else:  # pragma: no cover - parametrization is exhaustive
        raise AssertionError(mutation)
    path = tmp_path / f"frozen-formal-{mutation}.json"
    path.write_text(json.dumps(evidence), encoding="utf-8")

    with pytest.raises(_MODULE.ValidationError, match=message):
        _MODULE.validate_evidence(path)


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
                    "support_contact_ordered_summary_verified": True,
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
                        "worker_sweep": [
                            {
                                "workers_requested": 1,
                                "decision_match": True,
                                "cold_snapshot_sync": {
                                    "wall_us": 120.0,
                                    "process_cpu_us": 115.0,
                                    "allocations": 10,
                                },
                            },
                            {
                                "workers_requested": 2,
                                "decision_match": True,
                                "ordered_contact_summary_used": True,
                                "cold_snapshot_sync": {
                                    "wall_us": 90.0,
                                    "process_cpu_us": 170.0,
                                    "allocations": 20,
                                },
                            },
                        ],
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
    assert (
        records[1]["details"]["support_contact_ordered_summary_verified"]
        is True
    )


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("truthy_passed", "passed must be a boolean"),
        ("truthy_enabled", "enabled must be a boolean"),
        ("missing_decision", "decision_match must be a boolean"),
        ("truthy_decision", "decision_match must be a boolean"),
        ("nested_truthy_decision", "decision_match must be a boolean"),
        ("nonfinite_latency", "must be finite and non-negative"),
        ("negative_nested_latency", "must be non-negative"),
        ("coercible_runs", "runs must be a non-negative integer"),
    ],
)
def test_engine_matrix_translation_rejects_malformed_pass_evidence(
    mutation: str, message: str
) -> None:
    fixture = {
        "fixture": "support",
        "passed": True,
        "runtime_query": {
            "revision_change": {
                "p50_us": 1.0,
                "p95_us": 2.0,
                "p99_us": 3.0,
                "max_us": 4.0,
                "allocations": 0,
            }
        },
        "validation": {
            "enabled": True,
            "runs": 9,
            "decision_match": True,
        },
        "noncollision_validation": {"enabled": False},
    }
    if mutation == "truthy_passed":
        fixture["passed"] = "true"
    elif mutation == "truthy_enabled":
        fixture["validation"]["enabled"] = "false"
    elif mutation == "missing_decision":
        del fixture["validation"]["decision_match"]
    elif mutation == "truthy_decision":
        fixture["validation"]["decision_match"] = "false"
    elif mutation == "nested_truthy_decision":
        fixture["validation"]["worker_sweep"] = [
            {"workers_requested": 1, "decision_match": "true"}
        ]
    elif mutation == "nonfinite_latency":
        fixture["runtime_query"]["revision_change"]["p95_us"] = float("nan")
    elif mutation == "negative_nested_latency":
        fixture["validation"]["worker_sweep"] = [
            {"workers_requested": 1, "wall_us": -1.0}
        ]
    elif mutation == "coercible_runs":
        fixture["validation"]["runs"] = "9"
    else:  # pragma: no cover - parametrization is exhaustive
        raise AssertionError(mutation)

    with pytest.raises(ValueError, match=message):
        _RUNNER._engine_matrix_records({"fixtures": [fixture]})


def test_engine_matrix_translation_bounds_fixture_rows() -> None:
    fixture = {
        "fixture": "support",
        "passed": True,
        "validation": {"enabled": False},
        "noncollision_validation": {"enabled": False},
    }

    with pytest.raises(ValueError, match="4096-row bound"):
        _RUNNER._engine_matrix_records(
            {
                "fixtures": (
                    fixture for _ in range(_RUNNER.MAX_FIXTURE_ROWS + 1)
                )
            }
        )


def _ordered_summary_validation_record() -> dict:
    worker_sweep = [
        {
            "workers_requested": 1,
            "decision_match": True,
            "cold_snapshot_sync": {
                "wall_us": 100.0,
                "process_cpu_us": 95.0,
                "allocations": 10,
            },
        },
        {
            "workers_requested": 2,
            "decision_match": True,
            "ordered_contact_summary_used": True,
            "cold_snapshot_sync": {
                "wall_us": 75.0,
                "process_cpu_us": 140.0,
                "allocations": 20,
            },
        },
    ]
    return {
        "record_type": "trajectory_validation",
        "fixture_id": "support_contact_ordered_summary",
        "passed": True,
        "sample_count": 30,
        "details": {
            "collision_validation": {
                "enabled": True,
                "runs": 30,
                "waypoints": 96,
                "workers": 2,
                "decision_match": True,
                "serial_p95_us": 100.0,
                "parallel_p95_us": 80.0,
                "support_contact_active": True,
                "fallback_reason": "",
                "worker_sweep": worker_sweep,
                "waypoint_sweep": [
                    {"waypoints": 8, "worker_sweep": worker_sweep},
                    {"waypoints": 16, "worker_sweep": worker_sweep},
                ],
            },
            "noncollision_validation": {"enabled": False},
            "support_contact_ordered_summary_verified": True,
        },
    }


def test_evidence_validator_accepts_ordered_validation_crossover_sweep() -> None:
    _MODULE._validate_rp5_record(  # noqa: SLF001
        _ordered_summary_validation_record(),
        "record",
        formal=True,
    )


def test_evidence_validator_rejects_unordered_validation_crossover_sweep() -> None:
    record = _ordered_summary_validation_record()
    record["details"]["collision_validation"]["waypoint_sweep"].reverse()
    with pytest.raises(_MODULE.ValidationError, match="must be strictly ordered"):
        _MODULE._validate_rp5_record(  # noqa: SLF001
            record,
            "record",
            formal=True,
        )


def test_evidence_validator_rejects_formal_warm_only_worker_sweep() -> None:
    record = _ordered_summary_validation_record()
    del record["details"]["collision_validation"]["worker_sweep"][0][
        "cold_snapshot_sync"
    ]
    with pytest.raises(_MODULE.ValidationError, match="cold_snapshot_sync"):
        _MODULE._validate_rp5_record(  # noqa: SLF001
            record,
            "record",
            formal=True,
        )


def test_live_summary_omits_p99_for_thirty_sample_stratum() -> None:
    summary = _LIVE._summary(list(range(1, 31)))
    assert summary["p50"] == 15.5
    assert summary["p95"] == pytest.approx(28.55)
    assert summary["p99"] is None
    assert summary["max"] == 30.0


@pytest.mark.parametrize("values", ([True], [1.0], ["1"]))
def test_live_summary_requires_exact_bounded_integer_samples(values) -> None:
    with pytest.raises(TypeError, match="exact integer"):
        _LIVE._summary(values)


def test_live_summary_rejects_oversized_and_out_of_range_samples() -> None:
    with pytest.raises(ValueError, match="sample bound"):
        _LIVE._summary(range(_LIVE.MAX_LATENCY_SAMPLES + 1))
    with pytest.raises(ValueError, match="signed 64-bit"):
        _LIVE._summary([_LIVE.MAX_LATENCY_NS + 1])
    with pytest.raises(TypeError, match="exact bool"):
        _LIVE._summary([1], include_p99=1)


@pytest.mark.parametrize("percentile", (True, "0.5"))
def test_live_percentile_rejects_non_real_or_bool_percentiles(percentile) -> None:
    with pytest.raises(TypeError, match="real number, not bool"):
        _LIVE._percentile([1, 2], percentile)


def test_live_percentile_requires_finite_range_and_sorted_input() -> None:
    with pytest.raises(ValueError, match=r"finite and in \[0, 1\]"):
        _LIVE._percentile([1, 2], float("nan"))
    with pytest.raises(ValueError, match="sorted"):
        _LIVE._percentile([2, 1], 0.5)



# Historical producer-only tests are acceptance inputs, not runnable fallbacks.
# Source blobs:
#   offline producer: 1f07b69faea96950fbbdbc35a4d8e3e9913cc2b1
#   live producer:    5d31237c04a5c3266a50572159921eeb515aa385
# Historical test blob: 0935aa3246f28dc7457706ae37db386759cf415d
HISTORICAL_TEST_DEFINITION_COUNT = 74
RETAINED_HISTORICAL_TEST_DEFINITION_COUNT = 39
REMOVED_HISTORICAL_CASE_COHORTS = (
    {
        "cohort": "planner_event_and_kernel_completion",
        "historical_lines": "1240-1290",
        "case_names": (
            "test_live_planner_records_pair_by_serial_admission_and_move_identity",
            "test_live_success_requires_behavior_consumption_of_kernel_completion",
        ),
        "future_gates": ("I5/H0", "I6/N"),
        "acceptance": (
            "Rebuild serial-admission, move-correlation, and "
            "behavior-consumption evidence against owner-issued Program and "
            "Kernel facts."
        ),
    },
    {
        "cohort": "runtime_metrics_controller_progress_and_stop",
        "historical_lines": "1291-1418",
        "case_names": (
            "test_live_runtime_metrics_capture_is_complete_and_scoped",
            "test_live_runtime_metric_deltas_reject_counter_regression",
            "test_live_runtime_metrics_require_fresh_attributable_baseline",
            "test_live_controller_progress_tracker_retains_every_exact_transition",
            "test_live_controller_progress_tracker_fails_on_poll_gap",
        ),
        "future_gates": ("I8/P5", "I9/I"),
        "acceptance": (
            "Rebuild fresh scoped runtime counters and exact "
            "controller-progress evidence against final Stop and the "
            "registered Isaac manager."
        ),
    },
    {
        "cohort": "contact_policy_scene_projection",
        "historical_lines": "1419-1567",
        "case_names": (
            "test_live_step18_mode_projection_has_only_mode_valid_fields",
            "test_live_step18_mode_apply_captures_authored_and_resolved_identity",
            "test_live_step18_baseline_mode_preserves_legacy_precise_shape",
        ),
        "future_gates": ("I6/N", "I8/P5", "I9/I"),
        "acceptance": (
            "Rebuild contact-mode authoring, resolution, and evidence "
            "projection on addressed scene products and final Stop policy."
        ),
    },
    {
        "cohort": "failure_wait_sample_sequence_and_publication",
        "historical_lines": "1568-1872",
        "case_names": (
            "test_live_failed_run_retains_cycle_and_kernel_evidence",
            "test_live_failed_run_records_secondary_capture_errors",
            "test_live_wait_prepares_fresh_plan_after_nonfinal_success",
            "test_live_sample_reports_true_behavior_node_envelope",
            "test_live_event_sequence_tracker_accepts_cross_batch_producer_reordering",
            "test_live_event_sequence_gap_is_fail_closed_at_quiescence",
            "test_live_event_sequence_tracker_rejects_duplicate_or_regressed_values",
            "test_live_evidence_writes_are_atomic_and_never_clobber",
            "test_live_evidence_validation_failure_publishes_nothing",
        ),
        "future_gates": ("I5/H0", "I6/N", "I8/P5", "I9/I"),
        "acceptance": (
            "Rebuild fault-fast waits, event sequencing, node envelopes, "
            "and atomic live-evidence publication using the restored public "
            "facts."
        ),
    },
    {
        "cohort": "failed_output_identity",
        "historical_lines": "1873-1884",
        "case_names": ("test_live_failed_output_path_is_distinct_from_success_target",),
        "future_gates": ("I6/N", "I8/P5", "I9/I"),
        "acceptance": (
            "Reintroduce distinct failure artifacts only with the restored "
            "source-bound live producer."
        ),
    },
    {
        "cohort": "process_cpu_sampling",
        "historical_lines": "1916-1934",
        "case_names": ("test_live_process_cpu_interval_reports_conservative_bounds",),
        "future_gates": ("I9/I",),
        "acceptance": (
            "Reintroduce bounded process CPU evidence for the manager-owned "
            "live qualification process."
        ),
    },
    {
        "cohort": "python_runtime_model_view",
        "historical_lines": "1935-2234",
        "case_names": (
            "test_qualification_rma_view_uses_production_resolved_model_identity",
        ),
        "future_gates": ("I6/N",),
        "acceptance": (
            "Replace the deleted Python Robot Model Authority view with "
            "resource-addressed native model products."
        ),
    },
    {
        "cohort": "isaac_capture_and_replay",
        "historical_lines": "2235-2913",
        "case_names": (
            "test_capture_payload_resolves_base_frame_goal_and_validates_semantic_hash",
            "test_capture_support_contact_modes_are_mode_scoped",
            "test_capture_rejects_invalid_support_contact_mode_fields",
            "test_resolved_collision_policy_support_modes_are_canonical",
            "test_capture_wait_uses_active_drop_planning_window_and_fresh_snapshot",
            "test_capture_wait_fails_promptly_on_composer_fault",
            "test_capture_waits_for_complete_successful_drop_plan_record",
            "test_capture_correlates_planner_attempt_by_run_and_execution",
            "test_capture_simulation_reset_and_play_use_deployment_control_contract",
            "test_capture_requires_loaded_saved_artifacts_to_match",
            "test_live_row_force_reloads_exact_disk_artifacts",
            "test_captured_replay_rejects_changed_source_or_model_view",
            "test_frozen_rp5_0_capture_requires_exact_manifest_identity",
        ),
        "future_gates": ("I6/N", "I8/P5", "I9/I"),
        "acceptance": (
            "Rebuild capture, reset/play, artifact reload, provenance, and "
            "replay acceptance through the native, final-Stop, and registered "
            "Isaac owners."
        ),
    },
)


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
    assert _MODULE.cycle_failure_reason(status) == expected


def test_legacy_roboplan_producers_gate_before_argument_parsing(capsys) -> None:
    assert _RUNNER.main(["--not-a-real-offline-option"]) == 78
    assert _LIVE.main(["--not-a-real-live-option"]) == 78
    payloads = [
        json.loads(line)
        for line in capsys.readouterr().err.splitlines()
        if line.strip()
    ]
    assert [payload["historical_source_blob"] for payload in payloads] == [
        "1f07b69faea96950fbbdbc35a4d8e3e9913cc2b1",
        "5d31237c04a5c3266a50572159921eeb515aa385",
    ]
    assert all(payload["exit_code"] == 78 for payload in payloads)


def test_removed_roboplan_cases_have_exact_future_acceptance_owners() -> None:
    removed = [
        case
        for cohort in REMOVED_HISTORICAL_CASE_COHORTS
        for case in cohort["case_names"]
    ]
    assert len(removed) == (
        HISTORICAL_TEST_DEFINITION_COUNT
        - RETAINED_HISTORICAL_TEST_DEFINITION_COUNT
    )
    assert len(set(removed)) == len(removed)
    assert all(cohort["future_gates"] for cohort in REMOVED_HISTORICAL_CASE_COHORTS)
    assert all(cohort["acceptance"] for cohort in REMOVED_HISTORICAL_CASE_COHORTS)
