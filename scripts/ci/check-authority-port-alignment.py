#!/usr/bin/env python3
"""Reject deleted authority surfaces during the composable-cell relocation."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = (
    "manyforge_core",
    "manyforge_scene",
    "manyforge_deployment",
    "manyforge_component_manager",
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
    (re.compile(r"\bIPlannerSceneProjection\b"), "deleted planner-scene projection interface"),
    (re.compile(r"\bISceneAuthority\b"), "deleted Core scene-authority interface"),
    (
        re.compile(r"\b(?:class\s+SceneAuthority|manyforge::core::SceneAuthority)\b"),
        "deleted Core SceneAuthority owner",
    ),
    (re.compile(r"\bRoboPlanPlanningContext\b"), "deleted RoboPlanPlanningContext adapter"),
    (re.compile(r"\b(?:i|roboplan_)planning_context\b"), "deleted planning-context file/API token"),
    (re.compile(r"\bcollision_check_normalized\b"), "deleted planner collision authority/fallback"),
    (re.compile(r"\bexpand_trajectory_for_collision_validation\b"), "deleted combined validation helper"),
    (re.compile(r"\bsync_snapshot_from_context\b"), "deleted observer-side-effect compatibility helper"),
    (re.compile(r"\bplanning_context_state\b"), "deleted stop-snapshot authority label"),
    (re.compile(r"\bupdate_gripper_commands\b"), "deleted observer-driven gripper ticker"),
    (re.compile(r"\bupdate_ros_grippers\b"), "deleted ROS gripper ticker alias"),
    (re.compile(r"\bcommand_ros_gripper\b"), "deleted ROS-specific gripper command alias"),
    (re.compile(r"\bget_ros_gripper_status\b"), "deleted ROS-specific gripper status alias"),
    (re.compile(r"\bmark_ros_gripper_grasp_lost\b"), "deleted ROS-specific gripper evidence alias"),
    (re.compile(r"\b_advance_gripper_commands\b"), "deleted Python gripper progression helper"),
)

REQUIRED = (
    "manyforge_core/include/manyforge_core/ports/iplanner_backend.hpp",
    "manyforge_core/include/manyforge_core/ports/iobserved_robot_state_provider.hpp",
    "manyforge_core/include/manyforge_core/ports/irobot_kinematics.hpp",
    "manyforge_core/include/manyforge_core/ports/ireference_pose_resolver.hpp",
    "manyforge_core/include/manyforge_core/ports/itrajectory_validator.hpp",
    "manyforge_core/include/manyforge_core/ports/icollision_trajectory_projector.hpp",
    "manyforge_core/include/manyforge_core/ports/igripper_command_backend.hpp",
    "manyforge_core/include/manyforge_core/ports/imanager_scene_projection.hpp",
    "manyforge_core/include/manyforge_core/collision/i_collision_query_engine.hpp",
    "manyforge_core/include/manyforge_core/collision/i_collision_diagnostic_comparator.hpp",
    "manyforge_core/include/manyforge_core/logical_scene_identity.hpp",
    "manyforge_scene/include/manyforge_scene/canonical_empty_scene_owner.hpp",
    "manyforge_roboplan/include/manyforge_roboplan/retained_planning_model_stage.hpp",
)

DELETED = (
    "manyforge_core/include/manyforge_core/ports/iplanning_context.hpp",
    "manyforge_core/include/manyforge_core/ports/iplanner_scene_projection.hpp",
    "manyforge_core/include/manyforge_core/ports/iscene_authority.hpp",
    "manyforge_core/include/manyforge_core/scene_authority.hpp",
    "manyforge_core/src/scene_authority.cpp",
    "manyforge_roboplan/include/manyforge_roboplan/roboplan_planning_context.hpp",
    "manyforge_roboplan/src/roboplan_planning_context.cpp",
    "manyforge_roboplan/include/manyforge_roboplan/roboplan_projection_backend.hpp",
    "manyforge_roboplan/include/manyforge_roboplan/roboplan_focused_adapters.hpp",
    "manyforge_roboplan/python/bindings.cpp",
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
            failures.append(f"missing current authority/port file: {relative}")
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
        "Composable-cell authority relocation check passed "
        f"({len(authored_sources())} production files scanned)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
