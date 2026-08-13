from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PROTOTYPE_IMPORT = "verification.composable_cell_i1a"
PRODUCTION_ROOTS = (
    "exts",
    "manyforge_assistant_bridge",
    "manyforge_behavior/manyforge_behavior",
    "manyforge_collision",
    "manyforge_composer/backend",
    "manyforge_core",
    "manyforge_example_models",
    "manyforge_hmi",
    "manyforge_medkit_adapter",
    "manyforge_nodes",
    "manyforge_roboplan",
    "manyforge_cclc_msgs",
    "manyforge_ros",
    "manyforge_ros2_control_execution",
    "manyforge_ros_bringup",
    "manyforge_supervision",
    "manyforge_xarm_adapter",
    "manyforge_xarm_control",
    "scripts",
)
TEXT_SUFFIXES = {
    ".bash",
    ".cc",
    ".cpp",
    ".h",
    ".hpp",
    ".py",
    ".sh",
    ".yaml",
    ".yml",
}


def _production_text_files() -> list[Path]:
    files: list[Path] = []
    for relative in PRODUCTION_ROOTS:
        root = REPO_ROOT / relative
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
                files.append(path)
    return files


def test_production_sources_do_not_import_i1a_prototypes() -> None:
    offenders = [
        str(path.relative_to(REPO_ROOT))
        for path in _production_text_files()
        if PROTOTYPE_IMPORT in path.read_text(encoding="utf-8", errors="replace")
    ]

    assert offenders == []


def test_prototype_is_not_registered_as_a_runtime_or_public_package() -> None:
    registration_surfaces = (
        REPO_ROOT / "pyproject.toml",
        REPO_ROOT / "docker-compose.yaml",
        REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml",
        REPO_ROOT / "scripts" / "launch.sh",
    )

    offenders = [
        str(path.relative_to(REPO_ROOT))
        for path in registration_surfaces
        if path.exists()
        and "composable_cell_i1a"
        in path.read_text(encoding="utf-8", errors="replace")
    ]

    assert offenders == []
