"""Fail-closed tests for the unavailable Python Program launcher."""

from pathlib import Path
import inspect
import subprocess
import sys

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).parent.parent))

from scripts.launch import UNAVAILABLE_MESSAGE, create_kernel, main


def test_create_kernel_fails_before_deployment_or_factory_access() -> None:
    class PoisonDeployment:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"deployment was accessed: {name}")

    with pytest.raises(
        RuntimeError,
        match="composable_cell_kernel_client_binding_unavailable",
    ):
        create_kernel(PoisonDeployment())


def test_launcher_is_explicitly_unavailable_before_argument_parsing(capsys) -> None:
    assert main(["--not-a-real-option"]) == 78
    assert capsys.readouterr().err.strip() == UNAVAILABLE_MESSAGE
    assert "parse_args" not in inspect.getsource(main)


def test_ros_full_runtime_services_share_one_canonical_image() -> None:
    """Common tooling aliases share the selected official full image."""
    repo_root = Path(__file__).resolve().parents[2]
    compose = yaml.safe_load(
        (repo_root / "docker-compose.yaml").read_text(encoding="utf-8")
    )
    services = compose["services"]
    build_owner = services["ros-jazzy-full"]
    canonical_image = build_owner["image"]

    assert build_owner["build"]["dockerfile"] == "Dockerfile.ros-lyrical-full"
    assert canonical_image == "${MANYFORGE_ROS_FULL_IMAGE:-manyforge-ros-lyrical-full}"
    assert "composer-ros" not in services
    assert "isaac-topic-control" not in services


def test_real_lite6_base_keeps_its_temporary_qualification_hold() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    recipe = (repo_root / "Dockerfile.ros-jazzy-real-lite6").read_text(encoding="utf-8")
    helper = (repo_root / "scripts/xarm/build-real-lite6-image.sh").read_text(encoding="utf-8")
    assert "ARG MANYFORGE_ROS_FULL_BASE_IMAGE=manyforge-ros-jazzy-full:q44-20261010" in recipe
    assert "BASE_IMAGE=${MANYFORGE_REAL_LITE6_ROS_BASE_IMAGE:-${MANYFORGE_ROS_FULL_BASE_IMAGE:-manyforge-ros-jazzy-full:q44-20261010}}" in helper


@pytest.mark.parametrize(("environment", "expected"), [
    ({}, "manyforge-ros-jazzy-full:q44-20261010"),
    ({"MANYFORGE_ROS_FULL_IMAGE": "common-lyrical"}, "manyforge-ros-jazzy-full:q44-20261010"),
    ({"MANYFORGE_ROS_FULL_BASE_IMAGE": "legacy-qualified"}, "legacy-qualified"),
    ({"MANYFORGE_REAL_LITE6_ROS_BASE_IMAGE": "dedicated-qualified"}, "dedicated-qualified"),
    ({"MANYFORGE_ROS_FULL_BASE_IMAGE": "legacy-qualified",
      "MANYFORGE_REAL_LITE6_ROS_BASE_IMAGE": "dedicated-qualified"}, "dedicated-qualified"),
])
def test_real_lite6_base_override_compatibility(environment, expected):
    repo_root = Path(__file__).resolve().parents[2]
    helper = (repo_root / "scripts/xarm/build-real-lite6-image.sh").read_text(encoding="utf-8")
    assignment = next(line for line in helper.splitlines() if line.startswith("BASE_IMAGE="))
    # Evaluate the actual production assignment without source/Docker build effects.
    selected = subprocess.run(
        ["/bin/bash", "-c", assignment + '\nprintf "%s" "$BASE_IMAGE"'],
        env=environment, check=True, capture_output=True, text=True,
    )
    assert selected.stdout == expected
