import os
from pathlib import Path
import re
import subprocess

import yaml


_REPO_ROOT = Path(__file__).resolve().parents[1]
_SCRIPT = _REPO_ROOT / "scripts" / "test-all.sh"


def test_ros_free_roboplan_suite_explicitly_disables_ros2_control() -> None:
    script = _SCRIPT.read_text(encoding="utf-8")
    roboplan_body = script.split("run_roboplan_tests() {", maxsplit=1)[1].split(
        "run_pybind_tests() {", maxsplit=1
    )[0]

    assert "-DMANYFORGE_ENABLE_ROS2_CONTROL=OFF" in roboplan_body


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

    for name in ("scripts/test-ros.sh", "scripts/test-ros-full.sh"):
        ros_test = (_REPO_ROOT / name).read_text(encoding="utf-8")
        assert 'source "${WORKSPACE}/install/setup.bash"' not in ros_test
        assert "refuse the source-mounted workspace install overlay" in ros_test


def test_native_launcher_build_fails_synchronously_before_detached_start(
    tmp_path: Path,
) -> None:
    source = (
        _REPO_ROOT / "scripts" / "lib" / "compose.sh"
    ).read_text(encoding="utf-8")

    foreground = source.index("docker run --rm")
    detached = source.index("docker run -d --rm")
    assert foreground < detached
    assert "./scripts/test-all.sh cpp" in source[foreground:detached]
    assert "./scripts/test-all.sh roboplan" in source[foreground:detached]
    assert "./scripts/test-all.sh cpp" not in source[detached:]
    assert (
        "--static-dir /tmp/manyforge-build/frontend-cache/workspace/dist"
        in source[detached:]
    )

    calls = tmp_path / "docker-calls"
    completed = subprocess.run(
        [
            "bash",
            "-c",
            (
                f"source {str(_REPO_ROOT / 'scripts/lib/compose.sh')!r}\n"
                "prepare_native_composer_rt_docker_args() "
                "{ NATIVE_COMPOSER_RT_DOCKER_ARGS=(); }\n"
                "start_vllm_status_watcher() { :; }\n"
                "stop_vllm_status_watcher() { :; }\n"
                "container_running() { return 1; }\n"
                "resolve_native_composer_image() {\n"
                "  NATIVE_COMPOSER_IMAGE_ID='sha256:"
                + ("d" * 64)
                + "'\n"
                "  NATIVE_COMPOSER_BUILD_CACHE_VOLUME='cache'\n"
                "}\n"
                "docker() {\n"
                '  printf "%s\\n" "$*" >>"${CALLS}"\n'
                '  if [[ "$1" == "run" && "$2" == "--rm" ]]; then\n'
                "    return 42\n"
                "  fi\n"
                "  return 0\n"
                "}\n"
                "if start_composer_dev_container; then exit 20; fi\n"
                "if grep -q '^run -d ' \"${CALLS}\"; then exit 21; fi\n"
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
    assert completed.returncode == 0, completed.stderr


def test_standalone_bindings_build_and_link_current_ros_free_libraries() -> None:
    script = _SCRIPT.read_text(encoding="utf-8")
    bindings_body = script.split("run_pybind_tests() {", maxsplit=1)[1].split(
        "run_python_tests() {", maxsplit=1
    )[0]
    binding_cmake = (
        _REPO_ROOT / "manyforge_core" / "python" / "CMakeLists.txt"
    ).read_text(encoding="utf-8")

    assert 'build_manyforge_core "$core_build" OFF' in bindings_body
    assert (
        'build_manyforge_deployment "$deployment_build" OFF'
        in bindings_body
    )
    assert (
        '-DMANYFORGE_DEPLOYMENT_DIR="$deployment_build"'
        in bindings_body
    )
    assert "manyforge_deployment_archive" in binding_cmake
    assert (
        '"${MANYFORGE_DEPLOYMENT_DIR}/libmanyforge_deployment.a"'
        in binding_cmake
    )
    assert 'if [ ! -f "$core_build/libmanyforge_core.a" ]' not in bindings_body
    assert (
        'if [ ! -f "$deployment_build/libmanyforge_deployment.a" ]'
        not in bindings_body
    )


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
