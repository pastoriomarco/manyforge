import os
from pathlib import Path
import re
import subprocess

import yaml


_REPO_ROOT = Path(__file__).resolve().parents[1]
_SCRIPT = _REPO_ROOT / "scripts" / "test-all.sh"
_UNAVAILABLE_MESSAGE = (
    "Composable-cell runtime unavailable: native planning and execution adapters "
    "are intentionally removed during the active rebuild."
)


def test_roboplan_suite_qualifies_only_the_retained_product_consumer() -> None:
    script = _SCRIPT.read_text(encoding="utf-8")
    roboplan_body = script.split("run_roboplan_tests() {", maxsplit=1)[1].split(
        "run_pybind_tests() {", maxsplit=1
    )[0]

    assert "C++ retained RoboPlan product-consumer tests" in roboplan_body
    assert "MANYFORGE_ENABLE_ROS2_CONTROL" not in roboplan_body
    assert "manyforge_roboplan_py" not in roboplan_body
    assert "manyforge_roboplan/python" not in roboplan_body


def test_canonical_dev_tests_use_container_local_build_products() -> None:
    script = _SCRIPT.read_text(encoding="utf-8")
    compose = yaml.safe_load(
        (_REPO_ROOT / "docker-compose.yaml").read_text(encoding="utf-8")
    )
    local_ci = (
        _REPO_ROOT / ".github" / "workflows" / "local-ci.yml"
    ).read_text(encoding="utf-8")
    entrypoint = (_REPO_ROOT / "docker-entrypoint.sh").read_text(
        encoding="utf-8"
    )

    assert compose["services"]["dev"]["volumes"] == [".:/workspace"]
    assert compose["services"]["ros-jazzy"]["volumes"] == [".:/workspace"]
    assert compose["services"]["ros-jazzy-full"]["volumes"] == [".:/workspace"]
    assert compose["services"]["composer"]["volumes"] == [".:/workspace"]
    assert compose["services"]["composer-api"]["volumes"] == [".:/workspace"]
    assert "build-cache" not in compose["volumes"]
    assert 'cache_volume="manyforge-local-ci-build-' not in local_ci
    assert '-v "${cache_volume}:/tmp/manyforge-build"' not in local_ci
    assert '"${image_id}" \\' in local_ci
    assert "reset_stale_cmake_build_dir" not in script
    assert "reset_stale_python_build_dir" not in entrypoint
    assert "rm -rf" not in script
    assert 'BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"' in script
    assert 'cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL"' in script
    assert 'make -j"$BUILD_PARALLEL_LEVEL"' in script
    assert re.search(r"^\s*(?:cmake|make)\s", entrypoint, re.MULTILINE) is None
    assert "manyforge_roboplan/python" not in entrypoint
    assert "ROBOPLAN_BUILD" not in entrypoint

    for name in (
        "docker-entrypoint.ros-jazzy.sh",
        "docker-entrypoint.ros-jazzy-full.sh",
    ):
        ros_entrypoint = (_REPO_ROOT / name).read_text(encoding="utf-8")
        assert "reset_stale_python_build_dir" not in ros_entrypoint
        assert "rm -rf" not in ros_entrypoint
        assert (
            re.search(
                r"^\s*(?:cmake|make)\s", ros_entrypoint, re.MULTILINE
            )
            is None
        )
        assert 'MANYFORGE_SOURCE_WORKSPACE_INSTALL:-OFF}" = "ON"' in ros_entrypoint
        assert "manyforge_roboplan/python" not in ros_entrypoint
        assert "ROBOPLAN_BUILD" not in ros_entrypoint

    ros_test = (_REPO_ROOT / "scripts/test-ros.sh").read_text(encoding="utf-8")
    assert 'source "${WORKSPACE}/install/setup.bash"' not in ros_test
    assert "refuse the source-mounted workspace install overlay" in ros_test


def test_native_launcher_fails_before_any_container_or_stale_artifact_probe(
    tmp_path: Path,
) -> None:
    source = (
        _REPO_ROOT / "scripts" / "lib" / "compose.sh"
    ).read_text(encoding="utf-8")

    function_body = source.split("start_composer_dev_container() {", 1)[1].split(
        "stop_composer_dev_container() {", 1
    )[0]
    assert function_body.index(
        "manyforge_fail_composable_cell_runtime_unavailable"
    ) < function_body.index("docker run --rm")
    assert "manyforge_roboplan/python" not in source
    assert "test-all.sh roboplan" not in source

    calls = tmp_path / "docker-calls"
    completed = subprocess.run(
        [
            "bash",
            "-c",
            (
                f"source {str(_REPO_ROOT / 'scripts/lib/compose.sh')!r}\n"
                "docker() {\n"
                '  printf "%s\\n" "$*" >>"${CALLS}"\n'
                "  return 0\n"
                "}\n"
                "start_composer_dev_container\n"
            ),
        ],
        cwd=_REPO_ROOT,
        env={
            **os.environ,
            "CALLS": str(calls),
            "MANYFORGE_ROOT": str(_REPO_ROOT),
            "MANYFORGE_VLLM_STATUS_DIR": str(tmp_path / "vllm"),
        },
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 78
    assert completed.stderr.strip() == _UNAVAILABLE_MESSAGE
    assert not calls.exists()


def test_ros_runtime_wrappers_share_one_explicit_unavailable_boundary() -> None:
    wrappers = (
        "scripts/ros/build-ros2-control-runtime.sh",
        "scripts/ros/start-composer-ros.sh",
        "scripts/ros/start-lite6-real-robot-composer.sh",
        "scripts/test-ros-full.sh",
    )
    forbidden = (
        "manyforge_roboplan_py",
        "manyforge_roboplan/python",
        "MANYFORGE_ENABLE_ROS2_CONTROL",
        "MANYFORGE_ENABLE_QUALIFICATION_PROBES",
    )

    for relative in wrappers:
        path = _REPO_ROOT / relative
        source = path.read_text(encoding="utf-8")
        assert "composable-cell-runtime-availability.sh" in source
        assert "manyforge_fail_composable_cell_runtime_unavailable" in source
        assert not any(token in source for token in forbidden)
        completed = subprocess.run(
            ["bash", str(path)],
            cwd=_REPO_ROOT,
            env={**os.environ, "WORKSPACE": str(_REPO_ROOT)},
            capture_output=True,
            text=True,
            check=False,
        )
        assert completed.returncode == 78, relative
        assert completed.stderr.strip() == _UNAVAILABLE_MESSAGE


def test_lane_preflights_fail_before_docker_or_robot_side_effects(
    tmp_path: Path,
) -> None:
    cases = (
        ("scripts/lib/isaac.sh", "maybe_build_isaac_image"),
        ("scripts/lib/real_robot.sh", "real_robot_preflight"),
        ("scripts/lib/real_robot.sh", "start_real_robot_lane"),
    )

    for relative, function in cases:
        calls = tmp_path / f"{function}-calls"
        completed = subprocess.run(
            [
                "bash",
                "-c",
                (
                    f"source {str(_REPO_ROOT / relative)!r}\n"
                    "docker() { printf '%s\\n' \"$*\" >>\"${CALLS}\"; }\n"
                    f"{function}\n"
                ),
            ],
            cwd=_REPO_ROOT,
            env={
                **os.environ,
                "CALLS": str(calls),
                "MANYFORGE_ROOT": str(_REPO_ROOT),
            },
            capture_output=True,
            text=True,
            check=False,
        )
        assert completed.returncode == 78, relative
        assert completed.stderr.strip() == _UNAVAILABLE_MESSAGE
        assert not calls.exists()


def test_bindings_and_integration_suites_fail_explicitly_during_cutover() -> None:
    script = _SCRIPT.read_text(encoding="utf-8")
    bindings_body = script.split("run_pybind_tests() {", maxsplit=1)[1].split(
        "run_python_tests() {", maxsplit=1
    )[0]
    integration_body = script.split("run_integration_tests() {", maxsplit=1)[
        1
    ].split("run_bridge_tests() {", maxsplit=1)[0]

    for body in (bindings_body, integration_body):
        assert "manyforge_fail_composable_cell_runtime_unavailable" in body
        assert "pytest" not in body
        assert "cmake" not in body

    for suite in ("bindings", "integration"):
        completed = subprocess.run(
            ["bash", str(_SCRIPT), suite],
            cwd=_REPO_ROOT,
            env={**os.environ, "WORKSPACE": str(_REPO_ROOT)},
            capture_output=True,
            text=True,
            check=False,
        )
        assert completed.returncode == 78, suite
        assert completed.stderr.strip() == _UNAVAILABLE_MESSAGE


def test_deployment_package_participates_in_repository_version_guard() -> None:
    version_guard = (
        _REPO_ROOT / "scripts" / "ci" / "verify-version-sync.sh"
    ).read_text(encoding="utf-8")

    assert 'check "manyforge_deployment CMakeLists.txt"' in version_guard


def test_test_suites_are_never_skipped_from_a_head_only_success_stamp() -> None:
    script = _SCRIPT.read_text(encoding="utf-8")

    assert "MANYFORGE_TEST_STAMP" not in script
    assert ".manyforge-tested-" not in script


def test_isaac_runtime_cache_is_bound_to_immutable_image_id() -> None:
    helper = _REPO_ROOT / "scripts" / "lib" / "isaac.sh"
    image_id = "sha256:" + ("b" * 64)
    completed = subprocess.run(
        [
            "bash",
            "-lc",
            (
                f"source {str(helper)!r}; "
                f"isaac_build_cache_volume_for_image_id {image_id!r}"
            ),
        ],
        cwd=_REPO_ROOT,
        env=os.environ.copy(),
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.strip() == (
        "manyforge_ros-full-build-cache-" + ("b" * 64)
    )

    source = helper.read_text(encoding="utf-8")
    assert 'export MANYFORGE_ROS_FULL_IMAGE="${image_id}"' in source
    assert (
        'export MANYFORGE_ROS_FULL_BUILD_CACHE_VOLUME="${ISAAC_BUILD_CACHE_VOLUME}"'
        in source
    )
    assert '"${COMPOSE_CMD[@]}" up -d isaac-topic-control composer-ros' in source
    isaac_runtime = (
        _REPO_ROOT / "scripts" / "ros" / "launch-isaac-topic-based-control.sh"
    ).read_text(encoding="utf-8")
    assert (
        'MANYFORGE_ROS_OVERLAY="${MANYFORGE_ROS_OVERLAY:-'
        '${MANYFORGE_BUILD_DIR}/isaac-ros-overlay}"'
    ) in isaac_runtime
    assert 'rm -rf "${MANYFORGE_ROS_OVERLAY}"' not in isaac_runtime
    assert "MANYFORGE_SOURCE_DEVELOPMENT_ROS_OVERLAYS" in isaac_runtime

    pinned = subprocess.run(
        [
            "bash",
            "-lc",
            (
                "docker() {\n"
                "  [[ \"$1 $2 $3\" == "
                '"image inspect manyforge-ros-jazzy-full" ]]\n'
                f"  printf '%s\\n' {image_id!r}\n"
                "}\n"
                f"source {str(helper)!r}; "
                "pin_isaac_runtime_image; "
                "printf '%s|%s|%s\\n' "
                '"${ISAAC_RUNTIME_IMAGE_ID}" '
                '"${MANYFORGE_ROS_FULL_IMAGE}" '
                '"${MANYFORGE_ROS_FULL_BUILD_CACHE_VOLUME}"'
            ),
        ],
        cwd=_REPO_ROOT,
        env={
            key: value
            for key, value in os.environ.items()
            if key
            not in {
                "MANYFORGE_ROS_FULL_IMAGE",
                "MANYFORGE_ROS_FULL_BUILD_CACHE_VOLUME",
            }
        },
        capture_output=True,
        text=True,
        check=False,
    )

    assert pinned.returncode == 0, pinned.stderr
    assert pinned.stdout.strip() == (
        f"{image_id}|{image_id}|manyforge_ros-full-build-cache-"
        + ("b" * 64)
    )
