from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
PRODUCTION_ROOTS = (
    "manyforge_behavior",
    "manyforge_composer/backend",
    "manyforge_composer/frontend/src",
    "manyforge_core",
    "manyforge_deployment",
    "manyforge_headless",
    "manyforge_headless_api",
    "manyforge_program_runner",
    "manyforge_roboplan",
    "manyforge_ros2_control_execution",
    "manyforge_ros_bringup",
    "manyforge_xarm_control",
    "scripts",
)
TEXT_SUFFIXES = {
    ".bash",
    ".cc",
    ".cmake",
    ".cpp",
    ".h",
    ".hpp",
    ".json",
    ".py",
    ".sh",
    ".ts",
    ".tsx",
    ".xml",
    ".yaml",
    ".yml",
}
RETIRED_FRAGMENTS = (
    "manyforge_robot_lifecycle",
    "/api/robot-lifecycle",
    "RobotLifecycleCard",
    "LocalOperatorGateway",
    "XArmLifecycleRosPort",
    "lite6_execution_admission_provider",
    "lite6_lifecycle_command_evidence",
    "lite6_supervised_preflight",
    "execution_admission_provider_launch",
    "supervised_command_evidence",
)


def _production_files() -> list[Path]:
    result: list[Path] = []
    for relative_root in PRODUCTION_ROOTS:
        root = REPO_ROOT / relative_root
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if (
                path.is_file()
                and path.suffix in TEXT_SUFFIXES
                and "test" not in path.parts
                and "tests" not in path.parts
                and "__pycache__" not in path.parts
            ):
                result.append(path)
    return result


def test_legacy_lifecycle_packages_have_no_remaining_files() -> None:
    for relative_root in (
        "manyforge_robot_lifecycle",
        "manyforge_robot_lifecycle_msgs",
    ):
        root = REPO_ROOT / relative_root
        assert not root.exists() or not any(path.is_file() for path in root.rglob("*"))


def test_production_surfaces_do_not_retain_legacy_authority_facades() -> None:
    offenders: list[str] = []
    for path in _production_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for fragment in RETIRED_FRAGMENTS:
            if fragment in text:
                offenders.append(f"{path.relative_to(REPO_ROOT)}: {fragment}")

    assert offenders == []


def test_low_level_xarm_command_gate_is_removed_with_the_supervised_mode() -> None:
    # R7 package 5 (2026-10-25): the supervised xArm transport seam that kept
    # the renewable command gate was removed with the supervised mode.
    xarm_roots = (
        REPO_ROOT / "manyforge_xarm_control/include",
        REPO_ROOT / "manyforge_xarm_control/src",
    )
    actual = {
        path.relative_to(REPO_ROOT).as_posix()
        for root in xarm_roots
        for path in root.rglob("*")
        if path.is_file()
        and path.suffix in TEXT_SUFFIXES
        and "set_command_gate" in path.read_text(encoding="utf-8", errors="replace")
    }
    assert actual == set()
