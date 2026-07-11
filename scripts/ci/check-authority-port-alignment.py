#!/usr/bin/env python3
"""Reject regressions to the deleted planning-context authority monolith."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = (
    "manyforge_core",
    "manyforge_collision",
    "manyforge_roboplan",
    "manyforge_ros2_control_execution",
    "manyforge_behavior",
    "manyforge_composer/backend",
)
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".h", ".py", ".cmake", ".txt"}
PRUNED_PARTS = {
    ".git",
    "build",
    "dist",
    "node_modules",
    "test",
    "tests",
    "__pycache__",
}

FORBIDDEN = (
    (re.compile(r"\bIPlanningContext\b"), "deleted IPlanningContext interface"),
    (re.compile(r"\bRoboPlanPlanningContext\b"), "deleted RoboPlanPlanningContext adapter"),
    (re.compile(r"\b(?:i|roboplan_)planning_context\b"), "deleted planning-context file/API token"),
    (re.compile(r"\bcollision_check_normalized\b"), "deleted planner collision authority/fallback"),
    (re.compile(r"\bexpand_trajectory_for_collision_validation\b"), "deleted combined validation helper"),
    (re.compile(r"\bsync_snapshot_from_context\b"), "deleted observer-side-effect compatibility helper"),
    (re.compile(r"\bplanning_context_state\b"), "deleted stop-snapshot authority label"),
)

REQUIRED = (
    "manyforge_core/include/manyforge_core/ports/iplanner_backend.hpp",
    "manyforge_core/include/manyforge_core/ports/iplanner_scene_projection.hpp",
    "manyforge_core/include/manyforge_core/ports/iobserved_robot_state_provider.hpp",
    "manyforge_core/include/manyforge_core/ports/irobot_kinematics.hpp",
    "manyforge_core/include/manyforge_core/ports/ireference_pose_resolver.hpp",
    "manyforge_core/include/manyforge_core/ports/itrajectory_validator.hpp",
    "manyforge_core/include/manyforge_core/ports/icollision_trajectory_projector.hpp",
    "manyforge_core/include/manyforge_core/ports/iscene_authority.hpp",
    "manyforge_core/include/manyforge_core/collision/i_collision_query_engine.hpp",
    "manyforge_core/include/manyforge_core/collision/i_collision_diagnostic_comparator.hpp",
    "manyforge_core/include/manyforge_core/scene_authority.hpp",
    "manyforge_core/src/scene_authority.cpp",
    "manyforge_roboplan/include/manyforge_roboplan/roboplan_projection_backend.hpp",
    "manyforge_roboplan/include/manyforge_roboplan/roboplan_focused_adapters.hpp",
)

DELETED = (
    "manyforge_core/include/manyforge_core/ports/iplanning_context.hpp",
    "manyforge_roboplan/include/manyforge_roboplan/roboplan_planning_context.hpp",
    "manyforge_roboplan/src/roboplan_planning_context.cpp",
)


def authored_sources() -> list[Path]:
    files: list[Path] = []
    for relative_root in SOURCE_ROOTS:
        root = ROOT / relative_root
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or any(
                part in PRUNED_PARTS or part.startswith("build")
                for part in path.parts
            ):
                continue
            if path.name == "CMakeLists.txt" or path.suffix in SOURCE_SUFFIXES:
                files.append(path)
    return sorted(files)


def main() -> int:
    failures: list[str] = []
    for relative in REQUIRED:
        if not (ROOT / relative).is_file():
            failures.append(f"missing focused authority/port file: {relative}")
    for relative in DELETED:
        if (ROOT / relative).exists():
            failures.append(f"deleted authority file reintroduced: {relative}")

    for path in authored_sources():
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern, description in FORBIDDEN:
            if pattern.search(text):
                failures.append(
                    f"{path.relative_to(ROOT)}: contains {description} ({pattern.pattern})"
                )

    bindings = ROOT / "manyforge_roboplan/python/bindings.cpp"
    if bindings.is_file() and re.search(r"\bprojection_backend\b", bindings.read_text()):
        failures.append(
            "manyforge_roboplan/python/bindings.cpp: Python binding bypasses focused ports via projection_backend"
        )

    core_roots = (ROOT / "manyforge_core", ROOT / "manyforge_collision")
    for core_root in core_roots:
        for path in core_root.rglob("*"):
            if not path.is_file() or any(
                part in PRUNED_PARTS or part.startswith("build")
                for part in path.parts
            ):
                continue
            if path.suffix not in SOURCE_SUFFIXES and path.name != "CMakeLists.txt":
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if re.search(
                r"(?:#\s*include\s*[<\"]manyforge_roboplan/|"
                r"(?:target_link_libraries|find_package)\s*\([^)]*manyforge_roboplan)",
                text,
                re.DOTALL,
            ):
                failures.append(
                    f"{path.relative_to(ROOT)}: core/collision depends on RoboPlan adapter"
                )

    if failures:
        print("Authority and port alignment check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        f"Authority and port alignment check passed ({len(authored_sources())} production files scanned)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
