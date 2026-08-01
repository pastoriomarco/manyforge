"""Source contracts for installed pre-I10 native xArm entrypoint gates.

Related C++ foundations remain compiled for ordered I7/I10 porting and fake
evidence, but the installed executables must neither reach nor load them during
the relocation window. Historical blobs at ManyForge commit 082221... are:
preflight ``2dc16f13...8aaa``, lifecycle evidence ``3f3e34db...f12d``, and
admission provider ``b15a09a8...e70e``.
"""

import re
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    "relative_path",
    [
        "manyforge_xarm_control/src/lite6_supervised_preflight.cpp",
        "manyforge_xarm_control/src/lite6_lifecycle_command_evidence.cpp",
        "manyforge_xarm_control/src/lite6_execution_admission_provider.cpp",
    ],
)
def test_installed_xarm_main_fails_before_ros_or_command_work(
    relative_path: str,
) -> None:
    source = (REPO_ROOT / relative_path).read_text(encoding="utf-8")
    main = source.rsplit("int main", maxsplit=1)[1]

    assert "composable_cell_runtime_unavailable" in main
    assert "return 78" in main
    for forbidden in (
        "rclcpp::init",
        "parse_arguments",
        "execute(",
        "make_shared",
        ".spin(",
    ):
        assert forbidden not in main


@pytest.mark.parametrize(
    "target",
    [
        "lite6_supervised_preflight",
        "lite6_lifecycle_command_evidence",
        "lite6_execution_admission_provider",
    ],
)
def test_installed_xarm_gate_does_not_load_runtime_libraries(target: str) -> None:
    cmake = (REPO_ROOT / "manyforge_xarm_control/CMakeLists.txt").read_text(
        encoding="utf-8"
    )

    for command in ("target_link_libraries", "ament_target_dependencies"):
        assert re.search(rf"{command}\s*\(\s*{target}\b", cmake) is None
