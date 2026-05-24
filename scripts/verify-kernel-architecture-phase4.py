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

FORBIDDEN_PRODUCTION_TOKENS = {
    "legacy_collision_validation": "legacy collision validation compatibility flag",
    "collision_engine=direct_scene": "prototype collision engine selector",
    "direct_scene_roboplan": "adapter-hosted prototype engine id",
}

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
        if key == "live_isaac":
            live = load_optional_json(args.live_isaac_summary_json, failures, label)
            checks.append(check(
                live is not None and bool(live.get("success")),
                "live Isaac summary reports success",
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
    hits: dict[str, list[str]] = {token: [] for token in FORBIDDEN_PRODUCTION_TOKENS}
    for path in iter_text_files(repo_root):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for token in FORBIDDEN_PRODUCTION_TOKENS:
            if token in text:
                hits[token].append(str(path.relative_to(repo_root)))

    checks = []
    for token, description in FORBIDDEN_PRODUCTION_TOKENS.items():
        ok = not hits[token]
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
