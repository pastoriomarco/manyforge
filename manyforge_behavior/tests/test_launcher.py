"""Fail-closed tests for the unavailable Python Program launcher."""

from pathlib import Path
import inspect
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
    """Runtime service records share the image qualified by the launcher."""
    repo_root = Path(__file__).resolve().parents[2]
    compose = yaml.safe_load(
        (repo_root / "docker-compose.yaml").read_text(encoding="utf-8")
    )
    services = compose["services"]
    build_owner = services["ros-jazzy-full"]
    canonical_image = build_owner["image"]

    assert build_owner["build"]["dockerfile"] == "Dockerfile.ros-jazzy-full"
    assert canonical_image == "${MANYFORGE_ROS_FULL_IMAGE:-manyforge-ros-jazzy-full}"
    for service_name in ("composer-ros", "isaac-topic-control"):
        service = services[service_name]
        assert service["image"] == canonical_image
        assert "build" not in service
