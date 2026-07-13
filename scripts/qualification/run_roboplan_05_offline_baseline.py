#!/usr/bin/env python3
"""Run the version-neutral RP5 offline corpus inside a ManyForge container."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import site
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


def _repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


REPO_ROOT = _repo_root_from_script()
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "manyforge_behavior"))

from manyforge_behavior.model_paths import ResolvedRobotModel  # noqa: E402
from manyforge_behavior.robot_model_runtime import (  # noqa: E402
    RoboPlanRuntimeModelView,
    prepare_roboplan_runtime_model_view,
)
from scripts.qualification.roboplan_05_qualification import (  # noqa: E402
    EVIDENCE_SCHEMA,
    collect_inventory,
    require_source_inventory_stable,
    validate_corpus,
    validate_evidence,
    write_evidence_exclusive,
)


SUPPORTED_DIRECT_QUERY_CLASSES = {
    "binary_complete",
    "exact_distance",
    "runtime_binary",
    "runtime_distance",
}


def _read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def _run_json(command: list[str], *, env: dict[str, str]) -> dict[str, Any]:
    completed = subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise RuntimeError(
            f"expected one JSON line from {command[0]}, got {len(lines)}; "
            f"stderr={completed.stderr[-2000:]!r}"
        )
    value = json.loads(lines[0])
    if not isinstance(value, dict):
        raise RuntimeError(f"{command[0]} returned a non-object JSON value")
    return value


def _latency_us_to_ns(value: dict[str, Any]) -> dict[str, Any]:
    return {
        key: (None if item is None else float(item) * 1000.0)
        for key, item in value.items()
        if key in {"p50", "p95", "p99", "max", "mad"}
    }


def _engine_matrix_records(raw: dict[str, Any]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for fixture in raw.get("fixtures", []):
        fixture_id = f"neutral_engine/{fixture['fixture']}"
        runtime = fixture.get("runtime_query", {})
        revision = runtime.get("revision_change", {})
        if revision.get("max_us", 0.0) > 0.0:
            records.append(
                {
                    "record_type": "collision_revision",
                    "fixture_id": fixture_id,
                    "passed": bool(fixture["passed"]),
                    "latency_ns": _latency_us_to_ns(
                        {
                            "p50": revision.get("p50_us"),
                            "p95": revision.get("p95_us"),
                            "p99": revision.get("p99_us"),
                            "max": revision.get("max_us"),
                        }
                    ),
                    "details": {
                        "allocations": revision.get("allocations"),
                        "allocations_per_query": revision.get(
                            "allocations_per_query"
                        ),
                        "allocation_bound_per_query": revision.get(
                            "allocation_bound_per_query"
                        ),
                        "wcet_budget_ns": revision.get("wcet_budget_ns"),
                    },
                }
            )
        validation = fixture.get("validation", {})
        noncollision = fixture.get("noncollision_validation", {})
        if validation.get("enabled") or noncollision.get("enabled"):
            details = {
                "collision_validation": validation,
                "noncollision_validation": noncollision,
                "support_contact_fallback_verified": fixture.get(
                    "support_contact_fallback_verified", False
                ),
            }
            record: dict[str, Any] = {
                "record_type": "trajectory_validation",
                "fixture_id": fixture_id,
                "passed": bool(
                    fixture["passed"] and validation.get("decision_match", True)
                ),
                "sample_count": max(
                    int(validation.get("runs", 0)),
                    int(noncollision.get("runs", 0)),
                ),
                "details": details,
            }
            if noncollision.get("max_us", 0.0) > 0.0:
                record["latency_ns"] = _latency_us_to_ns(
                    {
                        "p50": noncollision.get("p50_us"),
                        "p95": noncollision.get("p95_us"),
                        "p99": noncollision.get("p99_us"),
                        "max": noncollision.get("max_us"),
                    }
                )
            records.append(record)
    return records


def _runtime_views(
    corpus: dict[str, Any], repo_root: Path
) -> dict[str, RoboPlanRuntimeModelView]:
    views: dict[str, RoboPlanRuntimeModelView] = {}
    for model_id, model in corpus["models"].items():
        if model["source"] != "manyforge_example_models":
            continue
        package_paths = [str((repo_root / path).resolve()) for path in model["package_paths"]]
        urdf_path = str((repo_root / model["urdf_path"]).resolve())
        resolved = ResolvedRobotModel(
            # Production resolve_robot_model() derives authority identity from
            # the resolved URDF stem. The corpus model_name remains the
            # planner-facing logical robot id and must not alter RMA hashes.
            model_name=Path(urdf_path).stem or "robot_model",
            urdf_path=urdf_path,
            srdf_path=str((repo_root / model["srdf_path"]).resolve()),
            package_paths=package_paths,
        )
        views[model_id] = prepare_roboplan_runtime_model_view(
            resolved_model=resolved
        )
    return views


def _model_override_args(
    model_id: str, views: dict[str, RoboPlanRuntimeModelView]
) -> list[str]:
    view = views.get(model_id)
    if view is None:
        return []
    result = ["--urdf", view.urdf_path, "--srdf", view.srdf_path]
    for package_path in view.package_paths:
        result.extend(["--package-path", package_path])
    return result


def _rma_records(
    corpus: dict[str, Any], views: dict[str, RoboPlanRuntimeModelView]
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for model_id, view in sorted(views.items()):
        mapping = view.import_view.canonical_to_view_joint_names
        inverse = view.import_view.view_to_canonical_joint_names
        bijective = len(mapping) == len(inverse) and all(
            inverse.get(view_name) == canonical
            for canonical, view_name in mapping.items()
        )
        public_joint_names = set(corpus["models"][model_id]["joint_names"])
        public_names_preserved = public_joint_names.issubset(mapping)
        records.append(
            {
                "record_type": "rma_contract",
                "fixture_id": model_id,
                "passed": bool(
                    bijective
                    and public_names_preserved
                    and Path(view.urdf_path).is_file()
                    and Path(view.srdf_path).is_file()
                ),
                "details": {
                    "canonical_fingerprint": view.authority.canonical_fingerprint,
                    "view_fingerprint": view.import_view.view_fingerprint,
                    "view_changed": view.import_view.changed,
                    "mapping_bijective": bijective,
                    "public_planning_joint_names_preserved": public_names_preserved,
                    "renamed_joint_count": sum(
                        canonical != view_name
                        for canonical, view_name in mapping.items()
                    ),
                },
            }
        )
    return records


def _verify_capture_provenance(
    *,
    capture_path: Path,
    repo_root: Path,
    view: RoboPlanRuntimeModelView | None,
) -> dict[str, Any]:
    capture = _read_json(capture_path)
    source = capture["source"]
    for field, digest_field in (
        ("program", "program_sha256"),
        ("deployment", "deployment_sha256"),
    ):
        artifact = (repo_root / source[field]).resolve()
        if not artifact.is_relative_to(repo_root) or not artifact.is_file():
            raise RuntimeError(f"captured source is unavailable: {source[field]}")
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        if digest != source[digest_field]:
            raise RuntimeError(
                f"captured {field} changed after capture; acquire a new fixture"
            )
    if view is None:
        raise RuntimeError("captured fixture requires a Robot Model Authority view")
    authority = capture["robot_model_authority"]
    if authority["canonical_fingerprint"] != view.authority.canonical_fingerprint:
        raise RuntimeError("captured canonical model fingerprint does not match runtime")
    if authority["view_fingerprint"] != view.import_view.view_fingerprint:
        raise RuntimeError("captured RoboPlan view fingerprint does not match runtime")
    return capture


def _source_and_environment(
    inventory: dict[str, Any], *, lane: str, command: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    source_inventory = inventory["source"]
    versions = source_inventory["roboplan_packages"]
    python_versions = source_inventory["python_packages"]
    roboplan_version = versions.get("roboplan", "unknown")
    source = {
        "manyforge_commit": source_inventory["manyforge_commit"],
        "manyforge_dirty": source_inventory["manyforge_dirty"],
        "roboplan_version": roboplan_version,
        "roboplan_ref": source_inventory["roboplan_ref"],
        "roboplan_commit": source_inventory["roboplan_commit"],
        "roboplan_package_versions": versions,
        "pinocchio_version": python_versions.get("libpinocchio")
        or python_versions.get("pin"),
        "coal_version": python_versions.get("libcoal")
        or python_versions.get("coal"),
        "eigenpy_version": python_versions.get("eigenpy"),
    }
    inventory_environment = inventory["environment"]
    affinity = inventory_environment.get("cpu_affinity", [])
    environment = {
        "lane": lane,
        "container": socket.gethostname(),
        "architecture": inventory_environment["architecture"],
        "cpu_count": inventory_environment["cpu_count"],
        "cpu_set": ",".join(str(cpu) for cpu in affinity) if affinity else None,
        "cpu_governor": ",".join(inventory_environment["cpu_governors"])
        or None,
        "viewer_stratum": "not_applicable",
        "command": command,
    }
    return source, environment


def _cmeel_runtime_library_paths() -> list[Path]:
    """Resolve cmeel libraries without pinning a Python minor version."""

    candidates = {
        Path(root).resolve() / "cmeel.prefix/lib"
        for root in site.getsitepackages()
    }
    user_site = site.getusersitepackages()
    if user_site:
        candidates.add(Path(user_site).resolve() / "cmeel.prefix/lib")
    paths = sorted(path for path in candidates if path.is_dir())
    if not paths:
        raise RuntimeError("no cmeel runtime library directory is available")
    return paths


def run(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    corpus_path = args.corpus.resolve()
    validate_corpus(
        corpus_path, allow_pending_capture=args.allow_pending_capture
    )
    corpus = _read_json(corpus_path)
    defaults = corpus["defaults"]
    build_dir = args.build_dir.resolve()
    collision_binary = build_dir / "perf_collision_strategy"
    planning_binary = build_dir / "perf_roboplan_planning_pipeline"
    probe_library = build_dir / "libroboplan_collision_counter_probe.so"
    for path in (collision_binary, planning_binary, probe_library):
        if not path.is_file():
            raise FileNotFoundError(f"required qualification target is missing: {path}")

    env = dict(os.environ)
    cmeel_libs = ":".join(str(path) for path in _cmeel_runtime_library_paths())
    env["LD_LIBRARY_PATH"] = f"{cmeel_libs}:{env.get('LD_LIBRARY_PATH', '')}"
    views = _runtime_views(corpus, repo_root)
    inventory = collect_inventory(repo_root)
    if inventory["source"]["manyforge_dirty"] and not args.allow_dirty_source:
        raise RuntimeError(
            "formal RP5-0 evidence requires a clean ManyForge source tree; "
            "use --allow-dirty-source only for an explicitly labeled preflight"
        )
    source, environment = _source_and_environment(
        inventory,
        lane=args.lane,
        command=args.command,
    )
    records = _rma_records(corpus, views)
    selected_fixture = args.fixture

    if not args.skip_engine_matrix and not selected_fixture:
        engine_matrix_binary = (
            build_dir
            / "manyforge_integration_tests/perf_collision_engine_qualification"
        )
        if not engine_matrix_binary.is_file():
            raise FileNotFoundError(
                f"required neutral-engine probe is missing: {engine_matrix_binary}"
            )
        raw_matrix = _run_json([str(engine_matrix_binary)], env=env)
        if not raw_matrix.get("all_passed"):
            raise RuntimeError("neutral collision-engine qualification failed")
        records.extend(_engine_matrix_records(raw_matrix))

    collision_warmup = args.collision_warmup or defaults["collision_warmup_queries"]
    collision_queries = args.collision_queries or defaults["collision_measured_queries"]
    collision_batches = args.collision_batches or defaults["collision_process_batches"]
    for fixture in corpus["collision_fixtures"]:
        if selected_fixture and fixture["id"] != selected_fixture:
            continue
        for query_class in fixture["query_classes"]:
            if query_class not in SUPPORTED_DIRECT_QUERY_CLASSES:
                continue
            for batch_index in range(collision_batches):
                command = [
                    str(collision_binary),
                    "--repo-root",
                    str(repo_root),
                    "--corpus",
                    str(corpus_path),
                    "--fixture",
                    fixture["id"],
                    "--query-class",
                    query_class,
                    "--warmup",
                    str(collision_warmup),
                    "--queries",
                    str(collision_queries),
                    *_model_override_args(fixture["model"], views),
                ]
                record = _run_json(command, env=env)
                record["batch_index"] = batch_index
                record["model_authority_view"] = (
                    "private" if fixture["model"] in views else "upstream_canonical"
                )
                records.append(record)

    planning_warmup = args.planning_warmup or defaults["planning_warmup_repetitions"]
    planning_repetitions = (
        args.planning_repetitions or defaults["planning_measured_repetitions"]
    )
    for fixture in corpus["planning_fixtures"]:
        if selected_fixture and fixture["id"] != selected_fixture:
            continue
        capture_file = fixture.get("capture_file")
        if capture_file and not (corpus_path.parent / capture_file).is_file():
            if args.allow_pending_capture:
                continue
            raise FileNotFoundError(
                f"captured planning fixture is missing: {capture_file}"
            )
        capture = None
        if capture_file:
            capture = _verify_capture_provenance(
                capture_path=(corpus_path.parent / capture_file).resolve(),
                repo_root=repo_root,
                view=views.get(fixture["model"]),
            )
        base_command = [
            str(planning_binary),
            "--repo-root",
            str(repo_root),
            "--corpus",
            str(corpus_path),
            "--fixture",
            fixture["id"],
            "--warmup",
            str(planning_warmup),
            "--repetitions",
            str(planning_repetitions),
            "--rng-seed",
            str(defaults["rng_seed"]),
            *_model_override_args(fixture["model"], views),
        ]
        primary = _run_json(base_command, env=env)
        if primary.get("collision_probe_available"):
            raise RuntimeError("primary planner run unexpectedly loaded collision probe")
        primary["measurement_mode"] = "primary"
        if capture is not None:
            primary["capture_source"] = {
                "semantic_sha256": capture["semantic_sha256"],
                "canonical_fingerprint": capture["robot_model_authority"][
                    "canonical_fingerprint"
                ],
                "view_fingerprint": capture["robot_model_authority"][
                    "view_fingerprint"
                ],
            }
        records.append(primary)
        probe_env = dict(env)
        probe_env["LD_PRELOAD"] = str(probe_library)
        attributed = _run_json(base_command, env=probe_env)
        if not attributed.get("collision_probe_available"):
            raise RuntimeError("planner attribution run did not load collision probe")
        if int(attributed.get("total_collision_query_count", 0)) <= 0:
            raise RuntimeError("planner attribution run observed no collision queries")
        if attributed.get("trajectory_hash_hex") != primary.get(
            "trajectory_hash_hex"
        ):
            raise RuntimeError("collision probe changed the planned trajectory")
        attributed["measurement_mode"] = "collision_probe_attribution"
        if capture is not None:
            attributed["capture_source"] = primary["capture_source"]
        records.append(attributed)

    evidence = {
        "schema_version": EVIDENCE_SCHEMA,
        "run_id": args.run_id
        or f"rp5-0-offline-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}",
        "phase": "rp5_0",
        "source": source,
        "environment": environment,
        "records": records,
        "inventory": {
            "build_references": inventory["build_references"],
            "capabilities": inventory["capabilities"],
            "workarounds": inventory["workarounds"],
        },
    }
    if not records or not all(record.get("passed") is True for record in records):
        raise RuntimeError("one or more qualification records failed")
    require_source_inventory_stable(inventory, collect_inventory(repo_root))
    environment["source_verified_after_run"] = True
    return evidence


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument(
        "--corpus",
        type=Path,
        default=REPO_ROOT / "benchmarks/roboplan_05/corpus/corpus-v1.json",
    )
    parser.add_argument(
        "--build-dir", type=Path, default=Path("/tmp/manyforge-build/roboplan")
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-id")
    parser.add_argument("--lane", default="ros_isaac_container")
    parser.add_argument("--fixture")
    parser.add_argument("--collision-warmup", type=int)
    parser.add_argument("--collision-queries", type=int)
    parser.add_argument("--collision-batches", type=int)
    parser.add_argument("--planning-warmup", type=int)
    parser.add_argument("--planning-repetitions", type=int)
    parser.add_argument("--skip-engine-matrix", action="store_true")
    parser.add_argument(
        "--allow-dirty-source",
        action="store_true",
        help="permit an explicitly labeled development preflight on dirty source",
    )
    parser.add_argument(
        "--allow-pending-capture",
        action="store_true",
        help="permit development smoke runs before the required live capture exists",
    )
    effective_argv = list(sys.argv[1:] if argv is None else argv)
    args = parser.parse_args(effective_argv)
    args.command = shlex.join(
        [sys.executable, str(Path(__file__).resolve()), *effective_argv]
    )
    try:
        evidence = run(args)
        write_evidence_exclusive(
            args.output,
            evidence,
            validator=validate_evidence,
        )
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"RP5 offline baseline failed: {exc}", file=sys.stderr)
        return 2
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
