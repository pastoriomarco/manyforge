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


def test_live_runtime_metrics_capture_is_complete_and_scoped(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    payload = {
        key: 0
        for key in (
            _LIVE._KERNEL_RUNTIME_REQUIRED_METRICS
            | _LIVE._KERNEL_RUNTIME_COUNTER_METRICS
        )
    }
    payload["runtime_collision_lookahead_max_samples"] = 32
    payload["unrelated_metric"] = 99

    def request_json(_base_url: str, path: str, **_kwargs: object) -> dict:
        assert path == "/api/kernel/metrics"
        return payload

    monkeypatch.setattr(_LIVE, "_request_json", request_json)
    captured = _LIVE._kernel_runtime_metrics("http://composer")

    assert captured["runtime_collision_lookahead_max_samples"] == 32
    assert "controller_command_progress_samples_total" in captured
    assert "unrelated_metric" not in captured


def test_live_runtime_metric_deltas_reject_counter_regression() -> None:
    before = {key: 7 for key in _LIVE._KERNEL_RUNTIME_COUNTER_METRICS}
    after = {key: 9 for key in _LIVE._KERNEL_RUNTIME_COUNTER_METRICS}

    deltas = _LIVE._kernel_runtime_counter_deltas(before, after)
    assert set(deltas) == set(_LIVE._KERNEL_RUNTIME_COUNTER_METRICS)
    assert set(deltas.values()) == {2}

    after["runtime_collision_ticks_completed"] = 6
    with pytest.raises(
        RuntimeError, match="runtime_collision_ticks_completed.*regressed"
    ):
        _LIVE._kernel_runtime_counter_deltas(before, after)


def test_live_runtime_metrics_require_fresh_attributable_baseline() -> None:
    baseline = {
        key: 0 for key in _LIVE._KERNEL_RUNTIME_FRESH_BASELINE_METRICS
    }
    _LIVE._require_fresh_kernel_runtime_baseline(baseline)

    baseline["runtime_collision_lookahead_jobs_started"] = 1
    with pytest.raises(
        RuntimeError,
        match="fresh kernel metric baseline.*lookahead_jobs_started=1",
    ):
        _LIVE._require_fresh_kernel_runtime_baseline(baseline)


def test_live_controller_progress_tracker_retains_every_exact_transition() -> None:
    baseline = {
        "controller_command_progress_samples_total": 0,
        "controller_last_command_progress_latency_ns": 0,
        "controller_max_command_progress_latency_ns": 0,
    }
    tracker = _LIVE._ControllerProgressSampleTracker(baseline)
    tracker.observe(baseline, observed_monotonic_ns=10)
    tracker.observe(
        {
            "controller_command_progress_samples_total": 1,
            "controller_last_command_progress_latency_ns": 70,
            "controller_max_command_progress_latency_ns": 70,
        },
        observed_monotonic_ns=20,
    )
    final_metrics = {
        "controller_command_progress_samples_total": 2,
        "controller_last_command_progress_latency_ns": 50,
        "controller_max_command_progress_latency_ns": 70,
    }
    result = tracker.finalize(final_metrics)

    assert result["native_counter_delta"] == 2
    assert result["native_max_ns"] == 70
    assert result["latency_ns"]["max"] == 70.0
    assert [sample["latency_ns"] for sample in result["samples"]] == [70, 50]
    assert [sample["observed_monotonic_ns"] for sample in result["samples"]][0] == 20


def test_live_controller_progress_tracker_fails_on_poll_gap() -> None:
    tracker = _LIVE._ControllerProgressSampleTracker(
        {
            "controller_command_progress_samples_total": 0,
            "controller_last_command_progress_latency_ns": 0,
            "controller_max_command_progress_latency_ns": 0,
        }
    )
    with pytest.raises(RuntimeError, match="advanced by 2"):
        tracker.observe(
            {
                "controller_command_progress_samples_total": 2,
                "controller_last_command_progress_latency_ns": 50,
                "controller_max_command_progress_latency_ns": 70,
            }
        )


@pytest.mark.parametrize(
    ("mode", "expected"),
    [
        (
            "binary_contact",
            {
                "mode": "contact_release",
                "release_evidence_mode": "binary_contact",
                "enforce_max_start_penetration": False,
                "max_start_penetration_m": 0.0,
                "release_tolerance_m": 0.0,
                "min_release_progress_m": 0.0,
                "require_separation_by_end": True,
                "require_strictly_decreasing_penetration": False,
                "allow_recontact": False,
            },
        ),
        (
            "bounded_penetration",
            {
                "mode": "bounded_penetration",
                "max_contact_penetration_m": 0.03,
            },
        ),
    ],
)
def test_live_step18_mode_projection_has_only_mode_valid_fields(
    mode: str, expected: dict[str, object]
) -> None:
    precise = {
        "support_object_id": "isaac_table_low_327",
        "mode": "contact_release",
        "release_evidence_mode": "signed_penetration",
        "enforce_max_start_penetration": True,
        "max_start_penetration_m": 0.03,
        "release_tolerance_m": 0.001,
        "min_release_progress_m": 0.0,
        "require_separation_by_end": True,
        "require_strictly_decreasing_penetration": True,
        "allow_recontact": False,
    }

    projected = _LIVE._step18_contact_for_mode(precise, mode, 0.03)

    assert projected["support_object_id"] == "isaac_table_low_327"
    for key, value in expected.items():
        assert projected[key] == value
    if mode == "bounded_penetration":
        assert "release_evidence_mode" not in projected
        assert "require_separation_by_end" not in projected
    else:
        assert "max_contact_penetration_m" not in projected


def test_live_step18_mode_apply_captures_authored_and_resolved_identity(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    precise = {
        "support_object_id": "isaac_table_low_327",
        "mode": "contact_release",
        "release_evidence_mode": "signed_penetration",
        "enforce_max_start_penetration": True,
        "max_start_penetration_m": 0.03,
        "release_tolerance_m": 0.001,
        "min_release_progress_m": 0.0,
        "require_separation_by_end": True,
        "require_strictly_decreasing_penetration": True,
        "allow_recontact": False,
    }
    saved = {
        "sceneResources": [
            {
                "objectId": "isaac_cheese_box",
                "metadata": {"support_contacts": [precise]},
            },
            {"objectId": "isaac_table_low_327"},
        ]
    }
    effective: dict[str, object] = {}

    def request_json(
        _base_url: str,
        path: str,
        *,
        method: str = "GET",
        payload: dict | None = None,
        **_kwargs: object,
    ) -> dict:
        if path == "/api/program/saved":
            return saved
        if path == "/api/session/apply":
            assert method == "POST" and payload is not None
            effective["sceneResources"] = payload["scene"]
            return {"status": "ok"}
        if path == "/api/program":
            return effective
        if path == "/api/debug/qualification/scene-authority-snapshot":
            return {
                "revision": {"revision_id": 7, "content_hash": "scene-seven"},
                "transaction_sequence": 8,
                "scene_admission_epoch": 9,
                "topology_epoch": 10,
                "policy_epoch": 11,
                "configured_collision_policy": {
                    "policy_id": "default_free_space",
                    "revision": {
                        "revision_id": 12,
                        "content_hash": "policy-twelve",
                    },
                    "support_contacts": [
                        {
                            "pair": {
                                "body_a": "isaac_cheese_box",
                                "body_b": "isaac_table_low_327",
                            },
                            "mode": "contact_release",
                            "release_evidence_mode": "binary_contact",
                        }
                    ],
                },
            }
        raise AssertionError(path)

    monkeypatch.setattr(_LIVE, "_request_json", request_json)

    evidence = _LIVE._apply_step18_support_contact_mode(
        "http://composer", mode="binary_contact", bounded_penetration_m=0.03
    )

    assert evidence["mode"] == "binary_contact"
    assert evidence["bounded_penetration_m"] is None
    assert evidence["authored_contact"]["release_evidence_mode"] == "binary_contact"
    assert evidence["resolved_contact"]["release_evidence_mode"] == "binary_contact"
    assert evidence["saved_scene_sha256"] != evidence["effective_scene_sha256"]


def test_live_step18_baseline_mode_preserves_legacy_precise_shape() -> None:
    legacy = {
        "support_object_id": "isaac_table_low_327",
        "mode": "contact_release",
        "enforce_max_start_penetration": True,
        "max_start_penetration_m": 0.03,
        "min_separation_clearance_m": 0.0,
        "release_tolerance_m": 0.001,
        "min_release_progress_m": 0.0,
        "require_separation_by_end": True,
        "require_strictly_decreasing_penetration": True,
        "allow_recontact": False,
    }

    projected = _LIVE._step18_contact_for_mode(
        legacy, "current_all_active_distance", 0.03
    )
    resolved = _LIVE._resolved_step18_contact(
        {
            "configured_collision_policy": {
                "support_contacts": [
                    {
                        "pair": {
                            "body_a": "isaac_cheese_box",
                            "body_b": "isaac_table_low_327",
                        },
                        "mode": "contact_release",
                        "min_separation_clearance_m": 0.0,
                    }
                ]
            }
        },
        "current_all_active_distance",
    )

    assert projected == legacy
    assert resolved["min_separation_clearance_m"] == 0.0


def test_live_failed_run_retains_cycle_and_kernel_evidence(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    metrics = {
        key: 0
        for key in (
            _LIVE._KERNEL_RUNTIME_REQUIRED_METRICS
            | _LIVE._KERNEL_RUNTIME_COUNTER_METRICS
        )
    }

    def request_json(_base_url: str, path: str, **_kwargs: object) -> dict:
        if path == "/api/cycle/status":
            return {"state": "stopped", "errorCode": "execution_aborted"}
        if path == "/api/kernel/metrics":
            return metrics
        raise AssertionError(path)

    monkeypatch.setattr(_LIVE, "_request_json", request_json)
    evidence = _LIVE._failure_runtime_evidence("http://composer")

    assert evidence["cycle_status"]["errorCode"] == "execution_aborted"
    assert "runtime_collision_ticks_completed" in evidence["kernel_runtime_metrics"]
    assert "capture_errors" not in evidence


def test_live_failed_run_records_secondary_capture_errors(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def fail_request(*_args: object, **_kwargs: object) -> dict:
        raise ConnectionError("offline")

    monkeypatch.setattr(_LIVE, "_request_json", fail_request)

    evidence = _LIVE._failure_runtime_evidence("http://composer")

    assert set(evidence["capture_errors"]) == {
        "cycle_status",
        "kernel_runtime_metrics",
    }


def test_live_wait_prepares_fresh_plan_after_nonfinal_success(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    required_types = (
        "plan_started",
        "plan_completed",
        "validation_started",
        "validation_passed",
        "execute_started",
        "execute_completed",
        "acknowledge_completed",
    )

    def success_batch(execution_id: int, first_index: int) -> list[dict]:
        return [
            {
                "type": event_type,
                "execution_id": execution_id,
                "event_index": first_index + offset,
                "sequence": first_index + offset,
                "details": {"move_id": "drop_approach"},
            }
            for offset, event_type in enumerate(required_types)
        ]

    batches = [success_batch(1, 1), success_batch(2, 8)]

    def request_json(_base_url: str, path: str) -> dict:
        if path.startswith("/api/kernel/events"):
            batch = batches.pop(0)
            return {"events": batch}
        if path == "/api/cycle/status":
            return {"state": "running"}
        raise AssertionError(path)

    monkeypatch.setattr(_LIVE, "_request_json", request_json)
    monkeypatch.setattr(_LIVE, "_enforce_viewer_stratum", lambda *args, **kwargs: 0)
    invalidations: list[str] = []
    tracker = _LIVE._KernelSequenceTracker(0)
    events, cursor, invalidated_count = _LIVE._wait_for_successes(
        "http://composer",
        cursor=0,
        sequence_tracker=tracker,
        required_successes=2,
        timeout_s=1.0,
        viewer_stratum="disconnected",
        focused_attested=False,
        poll_period_s=0.0,
        prepare_next_fresh_plan=lambda: invalidations.append("clear"),
    )

    assert len(events) == 14
    assert cursor == 14
    assert invalidated_count == 1
    assert invalidations == ["clear"]


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
                **(
                    {
                        "details": {
                            "collision_workers_requested": 4,
                            "collision_workers_effective": 4,
                            "collision_workers_used": 4,
                            "collision_parallel_fallback_reason": "",
                            "collision_ordered_contact_summary_used": True,
                            "collision_validation_wall_ns": 190,
                        }
                    }
                    if event_type == "validation_passed"
                    else {}
                ),
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
        "collision_workers_requested": 4,
        "collision_workers_effective": 4,
        "collision_workers_used": 4,
        "collision_parallel_fallback_reason": "",
        "collision_ordered_contact_summary_used": True,
        "collision_validation_wall_ns": 190,
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
            "group_id": "arm",
            "base_frame": "base_link",
            "base_pose": {
                "position_m": [0.0, 0.0, 0.0],
                "orientation_rpy_deg": [0.0, 0.0, 90.0],
            },
            "joint_names": ["joint_a", "joint_b"],
            "joint_positions": {"joint_a": 0.25, "joint_b": -0.5},
            "state_source": "controller_state",
            "source_timestamp_ns": 100,
            "source_age_ns": 1_000_000,
        },
        "grippers": [
            {
                "gripper_id": "gripper",
                "joint_name": "finger_joint",
                "current_position": 0.66,
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
                            "release_evidence_mode": "signed_penetration",
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
                        "motion_type": "pose",
                        "target_mode": "relative",
                        "target_reference": {
                            "type": "object",
                            "id": "drop_box",
                        },
                        "offset_reference": {
                            "type": "frame",
                            "id": "base_link",
                        },
                        "pose_goal": {
                            "position_m": [0.0, 0.0, 0.4],
                            "orientation_rpy_deg": [0.0, 0.0, 0.0],
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
                        "release_evidence_mode": "signed_penetration",
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
                "type": "pose",
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
        "result": {
            "scene_revision": {
                "revision_id": 7,
                "content_hash": "scene-seven",
            },
            "trajectory": {
                "joint_names": ["joint_a", "joint_b"],
                "waypoints": [
                    {
                        "time_from_start_s": 0.0,
                        "positions_rad": [0.25, -0.5],
                        "velocities_rad_s": [0.0, 0.0],
                    },
                    {
                        "time_from_start_s": 1.0,
                        "positions_rad": [0.5, -0.25],
                        "velocities_rad_s": [0.0, 0.0],
                    },
                ],
            },
            "error": None,
        },
        "metrics": {
            "rng_base_seed": 12345,
            "rng_seed": 12345,
            "shortcut_rng_seed_argument": -12346,
            "planning_attempt_index": 0,
        },
    }


def _generated_capture(tmp_path: Path) -> dict:
    program_path = tmp_path / "example.program.yaml"
    deployment_path = tmp_path / "example.deployment.yaml"
    program_path.write_text("name: test\n", encoding="utf-8")
    deployment_path.write_text("name: test\n", encoding="utf-8")
    return _CAPTURE._capture_payload(
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


def _write_rehashed_capture(path: Path, payload: dict) -> None:
    unsigned = dict(payload)
    unsigned.pop("semantic_sha256", None)
    payload["semantic_sha256"] = hashlib.sha256(
        json.dumps(unsigned, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    path.write_text(json.dumps(payload), encoding="utf-8")


def test_capture_payload_resolves_base_frame_goal_and_validates_semantic_hash(
    tmp_path: Path,
) -> None:
    payload = _generated_capture(tmp_path)
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
    assert payload["planning_result"]["trajectory_sha256"] == hashlib.sha256(
        json.dumps(
            payload["planning_result"]["trajectory"],
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
    ).hexdigest()
    assert payload["scene"]["robot"]["collision_projection_seed"]["groups"] == [
        {
            "group_id": "arm",
            "joint_names": ["joint_a", "joint_b"],
            "positions_rad": [0.25, -0.5],
            "source": "exact_plan_request",
        },
        {
            "group_id": "gripper",
            "joint_names": ["finger_joint"],
            "positions_rad": [0.66],
            "source": "fresh_visual_snapshot_gripper",
        },
    ]
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
            "release_evidence_mode": "signed_penetration",
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


def test_capture_support_contact_modes_are_mode_scoped(tmp_path: Path) -> None:
    schema = json.loads(
        (
            _REPO_ROOT / "benchmarks/roboplan_05/schema/capture-v1.schema.json"
        ).read_text(encoding="utf-8")
    )
    validator = Draft202012Validator(schema)
    path = tmp_path / "capture.json"

    def validate_contact(contact: dict) -> None:
        payload = _generated_capture(tmp_path)
        cheese = next(
            item
            for item in payload["scene"]["objects"]
            if item["object_id"] == "isaac_cheese_box"
        )
        cheese["support_contacts"] = [contact]
        _write_rehashed_capture(path, payload)
        validator.validate(payload)
        _MODULE.validate_capture(path, required_attachment="isaac_cheese_box")

    # Omitted mode/evidence retains the authored Precise default.
    validate_contact({"support_object_id": "drop_box"})
    validate_contact(
        {
            "support_object_id": "drop_box",
            "mode": "contact_release",
            "release_evidence_mode": "binary_contact",
            "require_separation_by_end": True,
            "allow_recontact": False,
        }
    )
    validate_contact(
        {
            "support_object_id": "drop_box",
            "mode": "bounded_penetration",
            "max_contact_penetration_m": 0.004,
        }
    )


@pytest.mark.parametrize(
    ("contact", "message"),
    [
        (
            {
                "support_object_id": "drop_box",
                "mode": "contact_release",
                "release_evidence_mode": "binary_contact",
                "release_tolerance_m": 0.001,
            },
            "release_tolerance_m must be 0",
        ),
        (
            {
                "support_object_id": "drop_box",
                "mode": "bounded_penetration",
            },
            "max_contact_penetration_m must be a number",
        ),
        (
            {
                "support_object_id": "drop_box",
                "mode": "bounded_penetration",
                "max_contact_penetration_m": -0.001,
            },
            "max_contact_penetration_m must be non-negative",
        ),
        (
            {
                "support_object_id": "drop_box",
                "mode": "bounded_penetration",
                "max_contact_penetration_m": 0.004,
                "release_evidence_mode": "signed_penetration",
            },
            "fields are not valid for mode bounded_penetration",
        ),
        (
            {
                "support_object_id": "drop_box",
                "mode": "contact_release",
                "max_contact_penetration_m": 0.004,
            },
            "fields are not valid for mode contact_release",
        ),
        (
            {
                "support_object_id": "drop_box",
                "min_separation_clearance_m": 0.0,
            },
            "contains unknown fields",
        ),
    ],
)
def test_capture_rejects_invalid_support_contact_mode_fields(
    tmp_path: Path, contact: dict, message: str
) -> None:
    payload = _generated_capture(tmp_path)
    cheese = next(
        item
        for item in payload["scene"]["objects"]
        if item["object_id"] == "isaac_cheese_box"
    )
    cheese["support_contacts"] = [contact]
    path = tmp_path / "capture.json"
    _write_rehashed_capture(path, payload)

    schema = json.loads(
        (
            _REPO_ROOT / "benchmarks/roboplan_05/schema/capture-v1.schema.json"
        ).read_text(encoding="utf-8")
    )
    assert not Draft202012Validator(schema).is_valid(payload)
    with pytest.raises(_MODULE.ValidationError, match=message):
        _MODULE.validate_capture(path)


def test_resolved_collision_policy_support_modes_are_canonical() -> None:
    pair = {"body_a": "carried", "body_b": "table"}
    signed = {
        "pair": pair,
        "mode": "contact_release",
        "release_evidence_mode": "signed_penetration",
        "enforce_max_start_penetration": True,
        "max_start_penetration_m": 0.01,
        "release_tolerance_m": 0.001,
        "min_release_progress_m": 0.0,
        "require_separation_by_end": True,
        "require_strictly_decreasing_penetration": True,
        "allow_recontact": False,
    }
    binary = {
        **signed,
        "release_evidence_mode": "binary_contact",
        "enforce_max_start_penetration": False,
        "max_start_penetration_m": 0.0,
        "release_tolerance_m": 0.0,
        "require_strictly_decreasing_penetration": False,
    }
    bounded = {
        "pair": pair,
        "mode": "bounded_penetration",
        "max_contact_penetration_m": 0.004,
    }
    contact_process = {"pair": pair, "mode": "contact_process"}

    def policy(contact: dict) -> dict:
        return {
            "policy_id": "test",
            "revision": {"revision_id": 1, "content_hash": "hash"},
            "allowed_static_static_pairs": [],
            "allowed_touch_link_pairs": [],
            "support_contacts": [contact],
            "contact_grasps": [],
            "require_full_diagnostics": False,
        }

    schema = json.loads(
        (
            _REPO_ROOT / "benchmarks/roboplan_05/schema/capture-v1.schema.json"
        ).read_text(encoding="utf-8")
    )
    validator = Draft202012Validator(schema)
    checked_capture = json.loads(
        (
            _REPO_ROOT
            / "benchmarks/roboplan_05/corpus/captures/isaac-light-drop-approach-v1.json"
        ).read_text(encoding="utf-8")
    )
    for contact in (signed, binary, bounded, contact_process):
        _MODULE._validate_collision_policy(policy(contact), "policy")  # noqa: SLF001
        candidate = json.loads(json.dumps(checked_capture))
        candidate["scene"]["authority"]["configured_collision_policy"] = policy(
            contact
        )
        validator.validate(candidate)

    with pytest.raises(_MODULE.ValidationError, match="contains unknown fields"):
        _MODULE._validate_collision_policy(  # noqa: SLF001
            policy({**signed, "min_separation_clearance_m": 0.0}), "policy"
        )
    invalid_unknown = json.loads(json.dumps(checked_capture))
    invalid_unknown["scene"]["authority"]["configured_collision_policy"] = policy(
        {**signed, "min_separation_clearance_m": 0.0}
    )
    assert not validator.is_valid(invalid_unknown)
    with pytest.raises(
        _MODULE.ValidationError,
        match="fields are not valid for mode bounded_penetration",
    ):
        _MODULE._validate_collision_policy(  # noqa: SLF001
            policy({**bounded, "release_evidence_mode": "signed_penetration"}),
            "policy",
        )


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
            "programSessionGeneration": 7,
            "programTreeHash": "loaded-tree-hash",
        },
        "/api/program/saved": {
            "name": "isaac-light",
            "sourcePath": str(program_path),
            "programSessionGeneration": 0,
            "programTreeHash": "",
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
            "loaded_payload_sha256": _CAPTURE.loaded_program_payload_sha256(
                responses["/api/program/saved"]
            ),
        },
        "deployment": {
            "name": "isaac-light-ros",
            "path": str(deployment_path),
            "sha256": hashlib.sha256(deployment_path.read_bytes()).hexdigest(),
            "loaded_payload_sha256": _CAPTURE._canonical_json_sha256(  # noqa: SLF001
                responses["/api/deployment/saved/full"]
            ),
        },
        "matched_input_sha256": _CAPTURE._canonical_json_sha256(  # noqa: SLF001
            {
                "program": _CAPTURE._step18_matched_program_payload(  # noqa: SLF001
                    responses["/api/program/saved"]
                ),
                "deployment": _CAPTURE._step18_matched_deployment_payload(  # noqa: SLF001
                    responses["/api/deployment/saved/full"]
                ),
            }
        ),
    }

    responses["/api/program/tree"] = {"tree": {"id": "modified"}}
    with pytest.raises(RuntimeError, match="unsaved or runtime-overridden"):
        _CAPTURE.require_loaded_artifacts_match(
            "http://composer",
            program_path=program_path,
            deployment_path=deployment_path,
            expected_program_name="isaac-light",
        )

    responses["/api/program/tree"] = responses["/api/program/saved/tree"]
    responses["/api/program"]["description"] = "stale in-memory payload"
    with pytest.raises(RuntimeError, match="runtime-overridden content"):
        _CAPTURE.require_loaded_artifacts_match(
            "http://composer",
            program_path=program_path,
            deployment_path=deployment_path,
            expected_program_name="isaac-light",
        )


def test_live_row_force_reloads_exact_disk_artifacts(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    program = (tmp_path / "program.yaml").resolve()
    deployment = (tmp_path / "deployment.yaml").resolve()
    calls: list[tuple[str, str, dict, float]] = []
    response = {
        "name": "isaac-light",
        "sourcePath": str(program),
        "programSessionGeneration": 17,
        "programTreeHash": "tree-seventeen",
    }

    def request(
        _base_url: str,
        path: str,
        *,
        method: str,
        payload: dict,
        timeout_s: float,
    ) -> dict:
        calls.append((path, method, payload, timeout_s))
        return response

    monkeypatch.setattr(_LIVE, "_request_json", request)
    evidence = _LIVE._reload_matched_artifacts(  # noqa: SLF001
        "http://composer", program_path=program, deployment_path=deployment
    )

    assert calls == [
        (
            "/api/program/load",
            "POST",
            {
                "path": str(program),
                "deploymentPath": str(deployment),
                "forceDiscardOverrides": True,
            },
            60.0,
        )
    ]
    assert evidence["response_payload_sha256"] == (
        _CAPTURE.loaded_program_payload_sha256(response)
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


def test_frozen_rp5_0_capture_requires_exact_manifest_identity(
    tmp_path: Path,
) -> None:
    capture_path = (
        tmp_path
        / "benchmarks/roboplan_05/corpus/captures/isaac-light-drop-approach-v1.json"
    )
    manifest_path = (
        tmp_path
        / "benchmarks/roboplan_05/baselines/roboplan-0.4.0-manifest.json"
    )
    capture_path.parent.mkdir(parents=True)
    manifest_path.parent.mkdir(parents=True)
    capture = {
        "semantic_sha256": "c" * 64,
        "source": {
            "program": "examples/program.yaml",
            "program_sha256": "a" * 64,
            "deployment": "examples/deployment.yaml",
            "deployment_sha256": "b" * 64,
        },
        "robot_model_authority": {
            "canonical_fingerprint": "d" * 64,
            "view_fingerprint": "e" * 64,
        },
    }
    manifest = {
        "baseline_id": "rp5-0-roboplan-0.4.0-pinocchio-4",
        "status": "complete",
        "source": {
            "program": {
                "path": "examples/program.yaml",
                "sha256": "a" * 64,
            },
            "deployment": {
                "path": "examples/deployment.yaml",
                "sha256": "b" * 64,
            },
        },
        "evidence": {
            "captured_authority_fixture": (
                "../corpus/captures/isaac-light-drop-approach-v1.json"
            ),
            "captured_authority_fixture_semantic_sha256": "c" * 64,
        },
    }
    capture_path.write_text(json.dumps(capture), encoding="utf-8")
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    view = SimpleNamespace(
        authority=SimpleNamespace(canonical_fingerprint="d" * 64),
        import_view=SimpleNamespace(view_fingerprint="e" * 64),
    )

    assert _RUNNER._verify_capture_provenance(
        capture_path=capture_path,
        repo_root=tmp_path,
        view=view,
        source_policy="frozen_rp5_0_manifest",
    ) == capture

    capture["semantic_sha256"] = "f" * 64
    capture_path.write_text(json.dumps(capture), encoding="utf-8")
    with pytest.raises(RuntimeError, match="semantic hash changed"):
        _RUNNER._verify_capture_provenance(
            capture_path=capture_path,
            repo_root=tmp_path,
            view=view,
            source_policy="frozen_rp5_0_manifest",
        )
