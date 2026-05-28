#!/usr/bin/env python3
"""Verify Kernel Architecture Sprint Phase 4 qualification evidence.

This verifier is intentionally stricter than a unit test. It checks whether the
release-gate evidence exists, is internally consistent, and covers the required
fixture classes. Use ``--allow-incomplete`` for local/offline development runs;
release qualification must run without that flag.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


REQUIRED_FIXTURES = {
    "primitive_dense_ur5_gripper",
    "cylinder_dense_ur5_gripper",
    "attached_object_ur5_gripper",
    "unsupported_mesh_fail_closed",
    "isaac_like_primitive_cell",
    "support_contact_serial_fallback",
}

REQUIRED_EXTERNAL_EVIDENCE = {
    "race": "ThreadSanitizer / race evidence",
    "soak": "CPU-reservation soak evidence",
    "interrupt": "stop/reset/interrupt qualification evidence",
    "visual": "visual-state snapshot polling under validation load",
    "live_isaac": "live Isaac scene qualification evidence",
}

REQUIRED_LIVE_ISAAC_PHASE4_P3_CHECKS = {
    "runtime_profile_commanded_sim",
    "kernel_snapshot_authority_metadata",
    "scene_state_robot_authority_metadata",
    "streaming_collision_budget_configured",
    "joint_slider_accepted",
    "joint_spinner_input_accepted",
    "cartesian_gizmo_accepted",
    "cartesian_input_accepted",
    "gripper_open_close_commanded",
    "rejected_frame_recovery",
    "reset_home_command_stream_refresh",
}

# Each entry is a regex pattern (Python `re` syntax) plus a human-readable
# description. Patterns are matched with `re.MULTILINE | re.DOTALL`, so `\s+`
# correctly matches whitespace including newlines — important for catching
# multi-line C++ signatures where the legacy surfaces were declared.
#
# Patterns are checked as substrings (re.search). Use `\b` for word-boundary
# anchoring where an identifier could embed in a longer name (e.g. plain
# `CollisionPolicy` vs. `CollisionPolicySnapshot`).
FORBIDDEN_PRODUCTION_PATTERNS: list[tuple[str, str]] = [
    (r"legacy_collision_validation", "legacy collision validation compatibility flag"),
    (r"collision_engine=direct_scene", "prototype collision engine selector"),
    (r"direct_scene_roboplan", "adapter-hosted prototype engine id"),
    # Cleanup: legacy collision-validation surfaces that lost
    # production callers. Reintroducing any of these tokens in a production
    # scan-dir is a regression.
    (r"\bCollisionPolicy\b(?!Snapshot)",
     "deleted CollisionPolicy class (use CollisionPolicySnapshot)"),
    (r"\bcollision_policy_\b",
     "deleted ExecutionKernel collision_policy_ member"),
    (r"CollisionPolicy::handle_collision",
     "deleted binary-collision stop-policy method"),
    (r"\bset_engine_mismatch_emitter\b",
     "deleted string-only engine-mismatch callback (use set_engine_mismatch_record_emitter)"),
    (r"\brun_predicted_path_monitor_diagnostic\b",
     "deleted IPlanningContext-based diagnostic (use snapshot/engine path)"),
    (r"\brun_serial_fallback\b",
     "deleted IPlanningContext-based worker-pool helper"),
    # Multi-line signature catchers — \s+ matches newlines under DOTALL.
    (r"validate_trajectory\s*\(\s*IPlanningContext",
     "deleted IPlanningContext coordinator/worker-pool validate_trajectory overload (use snapshot/engine overload)"),
    (r"validate_state\s*\(\s*IPlanningContext",
     "deleted IPlanningContext coordinator validate_state overload"),
    (r"std::vector\s*<\s*IPlanningContext\s*\*\s*>",
     "deleted std::vector<IPlanningContext*> worker-pool argument type"),
    # Trajectory-sampler consolidation: the per-backend linear position
    # helpers were deleted; production code consumes the shared
    # `sample_trajectory` API from
    # `manyforge_core/include/manyforge_core/trajectory/trajectory_sampler.hpp`
    # directly.
    (r"\binterpolate_joint_positions\b",
     "deleted per-backend linear interpolation helper "
     "(use sample_trajectory from manyforge_core/trajectory/)"),
]

SCAN_DIRS = [
    "manyforge_core",
    "manyforge_collision",
    "manyforge_roboplan",
    "manyforge_behavior",
    "manyforge_composer",
    "manyforge_ros_bringup",
    "manyforge_ros2_control",
    "examples",
]

SKIP_SUFFIXES = {
    ".pyc",
    ".png",
    ".jpg",
    ".jpeg",
    ".gif",
    ".glb",
    ".obj",
    ".stl",
    ".dae",
    ".so",
    ".a",
    ".o",
}


def check(condition: bool, name: str, failures: list[str]) -> dict[str, Any]:
    if not condition:
        failures.append(name)
    return {"name": name, "passed": condition}


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def load_optional_json(
    path: Path | None,
    failures: list[str],
    label: str,
) -> dict[str, Any] | None:
    if path is None:
        failures.append(f"{label} summary artifact missing")
        return None
    if not path.exists():
        failures.append(f"{label} summary artifact not found: {path}")
        return None
    try:
        data = load_json(path)
    except (OSError, json.JSONDecodeError) as exc:
        failures.append(f"{label} summary artifact unreadable: {exc}")
        return None
    return data


def _is_structural_visual_load_evidence(payload: dict[str, Any] | None) -> bool:
    """Return True if `payload` looks like a real qualify-visual-state-live
    run rather than a synthetic status-only assertion.

    Strict release rejects `{"success": true}`-only blobs because the
    harness produces them when an operator sets the legacy
    `MANYFORGE_PHASE4_VISUAL_LOAD_EVIDENCE=passed` env var without a
    real artifact path. Real artifacts carry sample counts, latency
    percentiles, and an empty failure list.
    """
    if not isinstance(payload, dict):
        return False
    if not bool(payload.get("success")):
        return False
    failures = payload.get("failures")
    if not isinstance(failures, list) or failures:
        return False
    try:
        duration_s = float(payload.get("duration_s", 0.0))
        hz = float(payload.get("hz", 0.0))
    except (TypeError, ValueError):
        return False
    if duration_s <= 0.0 or hz <= 0.0:
        return False
    summary = payload.get("summary")
    if not isinstance(summary, dict):
        return False
    try:
        sample_count = int(summary.get("sample_count", 0))
        error_count = int(summary.get("error_count", -1))
    except (TypeError, ValueError):
        return False
    if sample_count <= 0 or error_count != 0:
        return False
    latency = summary.get("latency_ms")
    if not isinstance(latency, dict):
        return False
    if "p95" not in latency:
        return False
    return True


def _phase4_p3_live_isaac_check_map(payload: dict[str, Any]) -> dict[str, bool]:
    checks = payload.get("checks")
    if not isinstance(checks, list):
        return {}
    out: dict[str, bool] = {}
    for item in checks:
        if not isinstance(item, dict):
            continue
        name = item.get("name")
        if not isinstance(name, str) or not name:
            continue
        out[name] = bool(item.get("passed"))
    return out


def _is_structural_live_isaac_p3_evidence(payload: dict[str, Any] | None) -> bool:
    """Return True only for a real Phase 4-P3 commanded-sim evidence blob.

    Strict Phase 4 used to accept any live Isaac JSON with ``success: true``.
    Phase 4-P3 tightened the live-Isaac gate: the evidence must prove the
    commanded-sim state-authority contract, direct-control recovery, gripper
    visual authority, and reset/home command-stream refresh. A status-only blob
    is no longer enough.
    """

    if not isinstance(payload, dict):
        return False
    if not bool(payload.get("success")):
        return False
    schema = str(payload.get("schema_version") or payload.get("schemaVersion") or "")
    if schema != "manyforge.phase4_p3_commanded_sim.v1":
        return False
    if str(payload.get("profile_kind") or payload.get("profileKind") or "") != "commanded_sim":
        return False
    if str(payload.get("authority") or "") != "backend_authored":
        return False
    check_map = _phase4_p3_live_isaac_check_map(payload)
    if not REQUIRED_LIVE_ISAAC_PHASE4_P3_CHECKS.issubset(check_map):
        return False
    return all(check_map[name] for name in REQUIRED_LIVE_ISAAC_PHASE4_P3_CHECKS)


def validate_collision_matrix(
    data: dict[str, Any],
    failures: list[str],
) -> list[dict[str, Any]]:
    checks: list[dict[str, Any]] = []
    fixtures = data.get("fixtures", [])
    fixtures_by_name = {fixture.get("fixture"): fixture for fixture in fixtures}

    checks.append(check(data.get("phase4_schema") == 1,
                        "collision matrix uses phase4_schema=1", failures))
    checks.append(check(bool(data.get("all_passed")),
                        "collision matrix reports all fixtures passed", failures))
    checks.append(check(REQUIRED_FIXTURES.issubset(fixtures_by_name.keys()),
                        "collision matrix covers every required fixture", failures))

    for name in sorted(REQUIRED_FIXTURES):
        fixture = fixtures_by_name.get(name)
        if fixture is None:
            continue
        checks.append(check(bool(fixture.get("passed")),
                            f"{name} fixture passed", failures))

    for name in (
        "primitive_dense_ur5_gripper",
        "cylinder_dense_ur5_gripper",
        "attached_object_ur5_gripper",
        "isaac_like_primitive_cell",
        "support_contact_serial_fallback",
    ):
        fixture = fixtures_by_name.get(name)
        if fixture is None:
            continue
        checks.append(check(bool(fixture.get("decision_match")),
                            f"{name} ManyForge/RoboPlan shadow match", failures))
        validation = fixture.get("validation", {})
        checks.append(check(bool(validation.get("decision_match")),
                            f"{name} serial/parallel validation match", failures))

    mesh_fixture = fixtures_by_name.get("unsupported_mesh_fail_closed")
    if mesh_fixture is not None:
        checks.append(check(bool(mesh_fixture.get("unsupported_fail_closed")),
                            "unsupported mesh fixture fails closed", failures))

    support_fixture = fixtures_by_name.get("support_contact_serial_fallback")
    if support_fixture is not None:
        checks.append(check(bool(support_fixture.get("support_contact_fallback_verified")),
                            "support-contact fixture uses observable serial fallback", failures))

    attached_fixture = fixtures_by_name.get("attached_object_ur5_gripper")
    if attached_fixture is not None:
        checks.append(check(int(attached_fixture.get("attached_object_count", 0)) > 0,
                            "attached-object fixture exports attached object", failures))

    return checks


def validate_external_evidence(args: argparse.Namespace, failures: list[str],
                               incomplete: list[str]) -> list[dict[str, Any]]:
    checks = []
    statuses = {
        "race": args.race_evidence,
        "soak": args.soak_evidence,
        "interrupt": args.interrupt_evidence,
        "visual": args.visual_evidence,
        "live_isaac": args.live_isaac_evidence,
    }
    for key, label in REQUIRED_EXTERNAL_EVIDENCE.items():
        status = statuses[key]
        if status == "not_run":
            incomplete.append(label)
        ok = status == "passed" or (args.allow_incomplete and status == "not_run")
        checks.append(check(ok, f"{label}: {status}", failures))
        if status != "passed":
            continue
        if key == "visual":
            visual = load_optional_json(args.visual_summary_json, failures, label)
            checks.append(check(
                visual is not None and bool(visual.get("success")),
                "visual-state live summary reports success",
                failures,
            ))
            load_evidence = load_optional_json(
                args.visual_load_evidence_json,
                failures,
                "visual validation-load evidence",
            )
            checks.append(check(
                load_evidence is not None and bool(load_evidence.get("success")),
                "visual validation-load evidence reports success",
                failures,
            ))
            # Strict mode also requires the structural fields a real
            # qualify-visual-state-live.py run emits — a status-only
            # {"success": true} blob (e.g. the synthetic file the harness
            # writes when MANYFORGE_PHASE4_VISUAL_LOAD_EVIDENCE=passed is
            # set without a real artifact path) must not satisfy the gate.
            # `--allow-incomplete` skips this stricter check so offline
            # development runs keep their existing semantics.
            if not args.allow_incomplete:
                checks.append(check(
                    _is_structural_visual_load_evidence(load_evidence),
                    "visual validation-load evidence carries structural probe fields",
                    failures,
                ))
        if key == "live_isaac":
            live = load_optional_json(args.live_isaac_summary_json, failures, label)
            checks.append(check(
                live is not None and bool(live.get("success")),
                "live Isaac summary reports success",
                failures,
            ))
            if not args.allow_incomplete:
                checks.append(check(
                    _is_structural_live_isaac_p3_evidence(live),
                    "live Isaac summary carries Phase 4-P3 commanded_sim evidence",
                    failures,
                ))
    return checks


def iter_text_files(root: Path):
    for relative in SCAN_DIRS:
        base = root / relative
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix.lower() in SKIP_SUFFIXES:
                continue
            yield path


def validate_release_cleanup(repo_root: Path, failures: list[str]) -> list[dict[str, Any]]:
    compiled = [
        (re.compile(pattern, re.MULTILINE | re.DOTALL), description)
        for pattern, description in FORBIDDEN_PRODUCTION_PATTERNS
    ]
    hits: list[list[str]] = [[] for _ in compiled]
    for path in iter_text_files(repo_root):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for index, (pattern, _) in enumerate(compiled):
            if pattern.search(text):
                hits[index].append(str(path.relative_to(repo_root)))

    checks = []
    for index, (_, description) in enumerate(compiled):
        ok = not hits[index]
        checks.append(check(ok, f"no production {description}", failures))
    return checks


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--collision-json", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--allow-incomplete", action="store_true")
    parser.add_argument("--race-evidence", choices=["passed", "failed", "not_run"],
                        default="not_run")
    parser.add_argument("--soak-evidence", choices=["passed", "failed", "not_run"],
                        default="not_run")
    parser.add_argument("--interrupt-evidence", choices=["passed", "failed", "not_run"],
                        default="not_run")
    parser.add_argument("--visual-evidence", choices=["passed", "failed", "not_run"],
                        default="not_run")
    parser.add_argument("--live-isaac-evidence", choices=["passed", "failed", "not_run"],
                        default="not_run")
    parser.add_argument("--visual-summary-json", type=Path)
    parser.add_argument("--visual-load-evidence-json", type=Path)
    parser.add_argument("--live-isaac-summary-json", type=Path)
    args = parser.parse_args()

    failures: list[str] = []
    checks: list[dict[str, Any]] = []
    incomplete: list[str] = []
    collision_data = load_json(args.collision_json)
    checks.extend(validate_collision_matrix(collision_data, failures))
    checks.extend(validate_external_evidence(args, failures, incomplete))
    checks.extend(validate_release_cleanup(args.repo_root, failures))

    report = {
        "phase4_schema": 1,
        "release_ready": not failures and not incomplete,
        "offline_checks_passed": not failures,
        "allow_incomplete": args.allow_incomplete,
        "collision_json": str(args.collision_json),
        "checks": checks,
        "incomplete_evidence": incomplete,
        "failures": failures,
    }

    rendered = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return 0 if not failures else 3


if __name__ == "__main__":
    sys.exit(main())
