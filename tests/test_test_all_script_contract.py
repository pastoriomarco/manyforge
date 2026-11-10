"""Cutover-boundary contracts for repository entry points and test lanes."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

import pytest
import yaml


_REPO_ROOT = Path(__file__).resolve().parents[1]
_TEST_ALL = _REPO_ROOT / "scripts" / "test-all.sh"
_BENCHMARK = _REPO_ROOT / "tests" / "test_benchmark.py"
_COLLISION_MAPPER_PERF = _REPO_ROOT / "scripts" / "run-collision-mapper-perf.sh"
_UNAVAILABLE_MESSAGE = (
    "composable_cell_runtime_unavailable: this legacy aggregate entrypoint has "
    "no single qualified current replacement; use the Component Manager, "
    "Headless, Program runner, and Composer entrypoints for maintained "
    "ManyForge-only runtime functions."
)
_BENCHMARK_UNAVAILABLE_MESSAGE = (
    "composable_cell_runtime_unavailable: the addressed native benchmark "
    "requires the build-qualified process fixture; run its RoboPlan process "
    "test in Docker."
)
_COLLISION_LOOKUP_DOCKER_MESSAGE = (
    "composable_cell_collision_lookup_probe_requires_docker: rerun with "
    "--in-container and MANYFORGE_BUILD_DIR set to an already qualified "
    "RoboPlan build"
)


@pytest.mark.parametrize("invocation", ((), ("all",)))
def test_historical_aggregate_test_shapes_fail_before_any_runner(
    invocation: tuple[str, ...],
    tmp_path: Path,
) -> None:
    marker = tmp_path / "external-effect"
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    for command in (
        "cmake",
        "ctest",
        "docker",
        "make",
        "npm",
        "nproc",
        "pytest",
        "python3",
    ):
        stub = fake_bin / command
        stub.write_text(
            f"#!/bin/sh\nprintf '%s\\n' called >> {str(marker)!r}\n",
            encoding="utf-8",
        )
        stub.chmod(0o755)

    completed = subprocess.run(
        ["bash", str(_TEST_ALL), *invocation],
        cwd=_REPO_ROOT,
        env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}"},
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 78
    assert completed.stdout == ""
    assert completed.stderr.strip() == _UNAVAILABLE_MESSAGE
    assert not marker.exists()


def test_reduced_aggregate_has_an_explicit_retained_selector() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    retained_body = source.split("    retained)", 1)[1].split("    *)", 1)[0]

    assert "./scripts/test-all.sh retained # current reduced aggregate" in source
    assert "    all)" not in source
    assert "run_cpp_tests" in retained_body
    assert "run_headless_tests" in retained_body
    assert "run_roboplan_tests" in retained_body
    assert "run_python_tests" in retained_body
    assert "run_observability_client_tests" in retained_body
    assert "run_bridge_tests" in retained_body
    assert "run_composer_backend_tests" in retained_body
    assert "run_composer_frontend_build" in retained_body
    assert "run_composer_frontend_tests" in retained_body
    assert "manyforge_headless_api/process_tests/" in source
    assert "manyforge_program_runner/tests/" in source
    assert "-p no:cacheprovider" in source
    assert "tests/test_public_contract_entrypoint_reconciliation.py" in source
    assert "tests/test_source_identity.py" in source
    assert "tests/test_grouped_gtest_runner.py" in source
    assert retained_body.count("run_roboplan_tests") == 1


def test_direct_benchmark_entrypoint_fails_before_product_import(
    tmp_path: Path,
) -> None:
    source = _BENCHMARK.read_text(encoding="utf-8")
    marker = tmp_path / "product-imported"
    poison_root = tmp_path / "poison"
    package = poison_root / "manyforge_behavior"
    package.mkdir(parents=True)
    (package / "__init__.py").write_text(
        (
            "from pathlib import Path\n"
            f"Path({str(marker)!r}).write_text('imported', encoding='utf-8')\n"
            "raise AssertionError('direct benchmark imported product code')\n"
        ),
        encoding="utf-8",
    )

    completed = subprocess.run(
        [sys.executable, str(_BENCHMARK)],
        cwd=tmp_path,
        env={**os.environ, "PYTHONPATH": str(poison_root)},
        capture_output=True,
        text=True,
        check=False,
    )

    assert source.index('if __name__ == "__main__":') < source.index("from manyforge_behavior")
    assert completed.returncode == 78
    assert completed.stdout == ""
    assert completed.stderr.strip() == _BENCHMARK_UNAVAILABLE_MESSAGE
    assert not marker.exists()


def test_collision_lookup_perf_successor_is_docker_only_and_executable() -> None:
    assert _COLLISION_MAPPER_PERF.stat().st_mode & 0o111 == 0o111

    completed = subprocess.run(
        [str(_COLLISION_MAPPER_PERF)],
        cwd=_REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 64
    assert completed.stdout == ""
    assert completed.stderr.strip() == _COLLISION_LOOKUP_DOCKER_MESSAGE
    source = _COLLISION_MAPPER_PERF.read_text(encoding="utf-8")
    assert "CompilerEmittedSymbolAndBodyLookupPerformanceStaysBounded" in source
    assert "perf_collision_mapper" not in source


def test_roboplan_suite_qualifies_only_the_retained_product_consumer() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    configure = source.split("configure_roboplan_build() {", 1)[1].split(
        "build_headless_roboplan_producer_closure() {", 1
    )[0]
    producer = source.split("build_headless_roboplan_producer_closure() {", 1)[1].split(
        "require_headless_runtime_packages() {", 1
    )[0]
    body = source.split("run_roboplan_tests() {", 1)[1].split("run_headless_cpp_tests() {", 1)[0]

    assert configure.index("codemodel-v2") < configure.index('cmake "$WORKSPACE')
    assert configure.index("cmakeFiles-v1") < configure.index('cmake "$WORKSPACE')
    assert configure.index("toolchains-v1") < configure.index('cmake "$WORKSPACE')
    assert "tail -" not in configure
    assert "source_ros_integration_environment" in configure
    assert "-DMANYFORGE_BUILD_ROS2_CONTROL_RUNTIME=ON" in configure
    assert 'bash "$WORKSPACE/scripts/ros/build-ros2-control-runtime.sh"' in producer
    assert "MANYFORGE_ROS2_CONTROL_BUILD_TESTING=ON" in producer
    assert 'MANYFORGE_ROBOPLAN_BUILD_DIR="$build"' in producer
    assert 'CMAKE_GENERATOR="$BUILD_GENERATOR"' in producer
    assert "MANYFORGE_ROS2_CONTROL_BUILD_KEEP_GOING=ON" in producer
    for target in (
        "manyforge_ros2_control_execution_host",
        "manyforge_ros2_control_execution_host_pick_place_planning_fixture",
        "manyforge_ros2_control_execution_host_projection_fixture",
        "manyforge_roboplan_candidate_worker",
    ):
        assert f"--target {target}" in producer
    assert 'source "$BUILD_DIR/isaac/ros-overlay/install/setup.bash"' in producer
    assert "tail -" not in producer
    assert "C++ retained RoboPlan product-consumer tests" in body
    assert "MANYFORGE_ENABLE_ROS2_CONTROL" not in body
    assert "manyforge_roboplan_py" not in body
    assert "manyforge_roboplan/python" not in body
    assert "configure_roboplan_build" in body
    assert (
        'cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL" -- "${BUILD_KEEP_GOING_ARGS[@]}"'
        in body
    )
    assert "tail -" not in body
    assert body.index("if ! cmake --build") < body.index('require_ctest_tests "manyforge_roboplan"')
    assert body.index('cd "$build"') < body.index('require_ctest_tests "manyforge_roboplan"')
    assert body.count("ctest --output-on-failure") == 1


def test_headless_required_process_cases_name_the_current_semantic_tests() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    body = source.split("run_headless_cpp_tests() {", 1)[1].split(
        "run_headless_python_tests() {", 1
    )[0]

    assert "ManagerDaemonProcess.CompatibleNativeReplacementUsesFreshEpochAfterCleanStop" in body
    assert "DistinctNativePackageUsesFreshManagerGenerationAfterCleanStop" not in body


@pytest.mark.parametrize("generator", [None, "Ninja", "Unix Makefiles"])
def test_headless_selector_builds_ros_producer_closure_before_headless(
    tmp_path: Path,
    generator: str | None,
) -> None:
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    build_root = tmp_path / "build"
    build_root.mkdir()
    marker = tmp_path / "events"
    workspace = tmp_path / "workspace"
    setup = tmp_path / "ros-setup.bash"
    setup.write_text('printf "%s\\n" ros-setup >> "$FAKE_LOG"\n')
    topic = tmp_path / "topic"
    data = tmp_path / "data"
    for install in (topic, data):
        install.mkdir()
        (install / "setup.bash").write_text("")
    (topic / "UPSTREAM_COMMIT").write_text(
        "aa9f9ef9525a60f8becea322537dd1d6d1bf9123\n")
    for relative in (
        "scripts/test-all.sh", "scripts/ros/build-ros2-control-runtime.sh",
        "scripts/lib/composable-cell-runtime-availability.sh",
    ):
        source = (_REPO_ROOT / relative).read_text()
        source = source.replace('"/opt/ros/${ROS_DISTRO:-lyrical}/setup.bash"', str(setup))
        source = source.replace('"/opt/ros/${ROS_DISTRO}/setup.bash"', str(setup))
        source = source.replace("/opt/manyforge/topic_based_ros2_control/install", str(topic))
        source = source.replace("/opt/manyforge/xarm_description/install", str(data))
        target = workspace / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source)
    for relative in (
        "manyforge_roboplan/CMakeLists.txt", "manyforge_ros2_control_execution/CMakeLists.txt",
        "manyforge_xarm_control/CMakeLists.txt", "manyforge_isaac_adapter/CMakeLists.txt",
        "manyforge_ros_bringup/package.xml",
    ):
        target = workspace / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.touch()
    test_all = workspace / "scripts/test-all.sh"

    (fake_bin / "cmake").write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        'if [ "${1:-}" = "--build" ]; then\n'
        '  root="$2"\n'
        '  if [ "$root" = "${MANYFORGE_BUILD_DIR}/roboplan" ]; then\n'
        "    printf '%s\\n' producer-build >> \"$FAKE_LOG\"\n"
        '    printf \'producer-targets:%s\\n\' "$*" >> "$FAKE_LOG"\n'
        "    for output in \\\n"
            "      manyforge_ros2_control_execution_host \\\n"
            "      manyforge_ros2_control_execution_host_pick_place_planning_fixture \\\n"
            "      manyforge_roboplan_candidate_worker; do\n"
        '      : > "$root/$output"\n'
        '      chmod +x "$root/$output"\n'
        "    done\n"
        '    mkdir -p "$root/ros_projection_host_fixture"\n'
        '    : > "$root/ros_projection_host_fixture/manyforge_ros2_control_execution_host"\n'
        '    chmod +x "$root/ros_projection_host_fixture/manyforge_ros2_control_execution_host"\n'
        '  elif [ "$root" = "${MANYFORGE_BUILD_DIR}/headless" ]; then\n'
        "    printf '%s\\n' headless-build >> \"$FAKE_LOG\"\n"
        '    mkdir -p "$root/manager-apply-test-fixture" \\\n'
        '      "$root/manager-apply-replacement-fixture" \\\n'
        '      "$root/assistant-native-test-fixture"\n'
        '    : > "$root/manager-apply-test-fixture/metadata.json"\n'
        '    : > "$root/manager-apply-replacement-fixture/metadata.json"\n'
        '    : > "$root/assistant-native-test-fixture/metadata.json"\n'
        '  elif [ "$root" = "${MANYFORGE_BUILD_DIR}/deployment" ]; then\n'
        '    package="$root/dual-robot-scenario-fixture/retained-package"\n'
        '    mkdir -p "$package"\n'
        '    : > "$package/canonical-empty-admission.dual_robot_wall_cell.json"\n'
        "  fi\n"
        "  exit 0\n"
        "fi\n"
        'if [ "$1" = "-S" ]; then\n'
        '  source="$2"; shift 2; root="$2"; mkdir -p "$root"; cd "$root"\n'
        '  selected_generator="${CMAKE_GENERATOR:-Unix Makefiles}"\n'
        '  previous=; for argument in "$@"; do\n'
        '    if [ "$previous" = "-G" ]; then selected_generator="$argument"; fi\n'
        '    previous="$argument"\n'
        '  done\n'
        '  printf \'CMAKE_GENERATOR:INTERNAL=%s\\n\' "$selected_generator" > CMakeCache.txt\n'
        "  printf '%s\\n' producer-configure >> \"$FAKE_LOG\"\n"
        '  printf \'producer-args:%s\\n\' "$source $*" >> "$FAKE_LOG"\n'
        '  exit 0\n'
        "fi\n"
        "printf 'CMAKE_GENERATOR:INTERNAL=%s\\n' \"$3\" > CMakeCache.txt\n"
        'case "$1" in\n'
        "  */manyforge_roboplan)\n"
        "    printf '%s\\n' producer-configure >> \"$FAKE_LOG\"\n"
        '    printf \'producer-args:%s\\n\' "$*" >> "$FAKE_LOG";;\n'
        "  */manyforge_headless)\n"
        "    printf '%s\\n' headless-configure >> \"$FAKE_LOG\"\n"
        '    printf \'headless-args:%s\\n\' "$*" >> "$FAKE_LOG";;\n'
        "esac\n",
        encoding="utf-8",
    )
    (fake_bin / "cmake").chmod(0o755)
    for name, body in (
        ("find", 'printf "%s\\n" "$FAKE_PINOCCHIO_CONFIG"\n'),
        ("ros2", 'case "$*" in\n'
                 '  "pkg prefix topic_based_ros2_control") printf "%s\\n" "$FAKE_TOPIC_INSTALL";;\n'
                 '  "pkg prefix xarm_description") printf "%s/xarm_description\\n" "$FAKE_DATA_INSTALL";;\n'
                 '  *) exit 98;;\nesac\n'),
        ("colcon", 'printf "ros-overlay-build\\n" >> "$FAKE_LOG"\n'
                   'while [ "$1" != "--install-base" ]; do shift; done\n'
                   'mkdir -p "$2"\n'
                   'printf \'printf "ros-overlay-source\\n" >> "$FAKE_LOG"\\n\' > "$2/setup.bash"\n'),
    ):
        (fake_bin / name).write_text("#!/bin/sh\nset -eu\n" + body)
        (fake_bin / name).chmod(0o755)
    (fake_bin / "ctest").write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        'printf \'ctest:%s\\n\' "$*" >> "$FAKE_LOG"\n'
        'if [ "${1:-}" = "-N" ]; then\n'
        "  printf '%s\\n' 'Test #1: ManagerDaemonProcess.RejectsPreManifestRootWithTypedStartupErrorBeforeIngress'\n"
        '  if [ "${FAKE_CTEST_OMIT_NATIVE:-0}" != 1 ]; then\n'
        "    printf '%s\\n' \\\n"
        "      'Test #2: ManagerDaemonProcess.CompatibleNativeReplacementUsesFreshEpochAfterCleanStop' \\\n"
        "      'Test #3: ManagerDaemonProcess.NamedApplyUsesTheRealManagedFenceAndSurvivesReplacement' \\\n"
        "      'Test #4: ManagerDaemonProcess.PhysicalCompositionStaysClosedAfterUnexpectedChildLossAndManagerReplacement'\n"
        "  fi\n"
        "  printf '%s\\n' 'Total Tests: 99'\n"
        "fi\n",
        encoding="utf-8",
    )
    (fake_bin / "ctest").chmod(0o755)
    (fake_bin / "nproc").write_text("#!/bin/sh\nprintf '1\\n'\n", encoding="utf-8")
    (fake_bin / "nproc").chmod(0o755)
    (fake_bin / "python3").write_text(
        "#!/bin/sh\n"
        "if [ \"${1:-}\" = '-c' ] || [ \"${1:-}\" = '-' ]; then\n"
        "  printf '/fake/python-package\\n'\n"
        "  exit 0\n"
        "fi\n"
        "exit 1\n",
        encoding="utf-8",
    )
    (fake_bin / "python3").chmod(0o755)
    (fake_bin / "pytest").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    (fake_bin / "pytest").chmod(0o755)

    environment = {
        **os.environ,
        "FAKE_LOG": str(marker),
        "MANYFORGE_BUILD_DIR": str(build_root),
        "PATH": f"{fake_bin}:{os.environ['PATH']}",
        "WORKSPACE": str(workspace),
        "MANYFORGE_VENV": str(tmp_path / "no-venv"),
        "FAKE_TOPIC_INSTALL": str(topic),
        "FAKE_DATA_INSTALL": str(data),
        "FAKE_PINOCCHIO_CONFIG": str(tmp_path / "cmeel.prefix/lib/cmake/pinocchio"),
    }
    environment.pop("CMAKE_GENERATOR", None)
    if generator is not None:
        environment["CMAKE_GENERATOR"] = generator
    completed = subprocess.run(
        ["bash", str(test_all), "headless"],
        cwd=_REPO_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0, completed.stdout + completed.stderr
    events = marker.read_text(encoding="utf-8").splitlines()
    assert events.index("producer-configure") < events.index("producer-build")
    assert events.index("producer-build") < events.index("ros-overlay-build")
    assert events.index("ros-overlay-build") < events.index("ros-overlay-source")
    assert events.index("ros-overlay-source") < events.index("headless-configure")
    assert events.index("producer-build") < events.index("headless-configure")
    assert events.index("headless-configure") < events.index("headless-build")
    producer_targets = next(event for event in events if event.startswith("producer-targets:"))
    assert "stop_qualification" not in producer_targets
    for target in (
        "manyforge_ros2_control_execution_host",
        "manyforge_ros2_control_execution_host_pick_place_planning_fixture",
        "manyforge_ros2_control_execution_host_projection_fixture",
        "manyforge_roboplan_candidate_worker",
    ):
        assert target in producer_targets
    expected_generator = generator or "Ninja"
    producer_args = next(event for event in events if event.startswith("producer-args:"))
    assert f"-G {expected_generator}" in producer_args
    assert producer_targets.endswith("-- -k 0" if expected_generator == "Ninja" else "-- -k")
    headless_args = next(event for event in events if event.startswith("headless-args:"))
    assert f"-G {expected_generator}" in headless_args
    assert (
        "-DMANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST="
        f"{build_root}/roboplan/manyforge_ros2_control_execution_host"
    ) in headless_args
    assert sum(event.startswith("ctest:--output-on-failure") for event in events) == 1

    omitted = subprocess.run(
        ["bash", str(test_all), "headless"],
        cwd=_REPO_ROOT,
        env={**environment, "FAKE_CTEST_OMIT_NATIVE": "1"},
        capture_output=True,
        text=True,
        check=False,
    )
    assert omitted.returncode != 0


@pytest.mark.parametrize("generator", ["Ninja", "Unix Makefiles"])
def test_keep_going_uses_real_configured_generator(tmp_path: Path, generator: str) -> None:
    project = tmp_path / "source"
    project.mkdir()
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(test_generator NONE)\n"
        "add_custom_target(probe ALL COMMAND ${CMAKE_COMMAND} -E true)\n",
        encoding="utf-8",
    )
    build = tmp_path / "build"
    subprocess.run(
        ["cmake", "-S", str(project), "-B", str(build), "-G", generator],
        check=True,
        capture_output=True,
        text=True,
    )
    # Exercise the actual helper against an actual cache/build tool. No
    # product build is needed to detect the original Make '-k 0' failure.
    source = _TEST_ALL.read_text(encoding="utf-8")
    helper = (
        "set_build_keep_going_args() {"
        + source.split("set_build_keep_going_args() {", 1)[1].split("\n}\n", 1)[0]
        + "\n}\n"
    )
    completed = subprocess.run(
        [
            "bash",
            "-eu",
            "-c",
            helper + '\nset_build_keep_going_args "$1"\n'
            'cmake --build "$1" -- "${BUILD_KEEP_GOING_ARGS[@]}"',
            "test",
            str(build),
        ],
        check=False,
        capture_output=True,
        text=True,
        env={**os.environ, "CMAKE_GENERATOR": "unrelated-shell-value"},
    )
    assert completed.returncode == 0, completed.stdout + completed.stderr
    incompatible = subprocess.run(
        [
            "cmake",
            "-S",
            str(project),
            "-B",
            str(build),
            "-G",
            "Unix Makefiles" if generator == "Ninja" else "Ninja",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert incompatible.returncode != 0
    assert "Does not match the generator used previously" in incompatible.stderr


def test_unsupported_generator_rejects_before_build(tmp_path: Path) -> None:
    completed = subprocess.run(
        ["bash", str(_TEST_ALL), "headless"],
        env={
            **os.environ,
            "CMAKE_GENERATOR": "unsupported",
            "MANYFORGE_BUILD_DIR": str(tmp_path / "build"),
        },
        check=False,
        capture_output=True,
        text=True,
    )
    assert completed.returncode == 2
    assert "Unsupported test build generator" in completed.stderr
    assert not (tmp_path / "build").exists()


def test_compose_keeps_toolchain_services_without_legacy_runtime_services() -> None:
    compose = yaml.safe_load((_REPO_ROOT / "docker-compose.yaml").read_text(encoding="utf-8"))
    services = compose["services"]

    for name in ("dev", "ros-jazzy", "ros-jazzy-full", "composer", "composer-api"):
        assert services[name]["volumes"] == [".:/workspace"]
    # The workbench (scripts/compose-dev.sh) is the only runtime route.
    assert "composer-ros" not in services
    assert "isaac-topic-control" not in services
    assert "volumes" not in compose


def test_current_build_lanes_use_fresh_container_local_products() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    local_ci = (_REPO_ROOT / ".github/workflows/local-ci.yml").read_text(encoding="utf-8")

    assert "MANYFORGE_TEST_STAMP" not in source
    assert ".manyforge-tested-" not in source
    assert "rm -rf" not in source
    assert 'BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"' in source
    assert 'cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL"' in source
    assert '"${image_id}" \\' in local_ci
    assert 'cache_volume="manyforge-local-ci-build-' not in local_ci


def test_hermes_gateway_matrix_cannot_activate_the_deleted_bridge() -> None:
    matrix_path = _REPO_ROOT / "scripts" / "hermes" / "managed-tool-gateway-matrix.json"
    payload = json.loads(matrix_path.read_text(encoding="utf-8"))
    source = matrix_path.read_text(encoding="utf-8")

    assert "manyforge" not in payload
    assert payload["_manyforge_cutover"]["available"] is False
    assert payload["_manyforge_cutover"]["historical_blob"] == (
        "e4097f2777ea45bcb3e9f09700aa357bcf378ad7"
    )
    assert "mcp_servers" not in source
    assert "host.openshell.internal" not in source


@pytest.mark.parametrize(
    "relative",
    (
        "scripts/ros/start-composer-ros.sh",
        "scripts/ros/start-lite6-real-robot-composer.sh",
        "scripts/test-ros-full.sh",
    ),
)
def test_ros_runtime_wrappers_share_one_explicit_unavailable_boundary(
    relative: str,
) -> None:
    path = _REPO_ROOT / relative
    source = path.read_text(encoding="utf-8")
    assert "composable-cell-runtime-availability.sh" in source
    assert "manyforge_fail_composable_cell_runtime_unavailable" in source
    assert "manyforge_roboplan_py" not in source

    completed = subprocess.run(
        ["bash", str(path)],
        cwd=_REPO_ROOT,
        env={**os.environ, "WORKSPACE": str(_REPO_ROOT)},
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 78
    assert completed.stderr.strip() == _UNAVAILABLE_MESSAGE


def test_ros2_control_builder_selects_only_current_runtime_products() -> None:
    source = (_REPO_ROOT / "scripts/ros/build-ros2-control-runtime.sh").read_text(encoding="utf-8")

    assert "manyforge_fail_composable_cell_runtime_unavailable" not in source
    assert "MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME=ON" in source
    assert "manyforge_ros2_control_execution_host" in source
    assert "manyforge_roboplan_candidate_worker" in source
    assert "manyforge_cclc_msgs" not in source
    assert "manyforge_ros_bringup" in source
    assert "--symlink-install" in source
    for forbidden in (
        "isaac_demo_supervisor",
        "manyforge_execution_host",
        "composer",
        "docker ",
    ):
        assert forbidden not in source



@pytest.mark.parametrize("name", ("Dockerfile.ros-jazzy", "Dockerfile.ros-jazzy-full", "Dockerfile.ros-lyrical-full"))
def test_ros_images_build_the_selected_picknik_source(name: str) -> None:
    source = (_REPO_ROOT / name).read_text(encoding="utf-8")
    assert "https://github.com/PickNikRobotics/topic_based_ros2_control.git" in source
    assert "aa9f9ef9525a60f8becea322537dd1d6d1bf9123" in source
    assert 'test "$(git rev-parse HEAD)"' in source
    assert "colcon build" in source
    assert source.index("/install/UPSTREAM_COMMIT") > source.index("colcon build")
    assert "COPY topic_based_ros2_control/" not in source
    if name == "Dockerfile.ros-lyrical-full":
        # Built in the clean ROS workspace stage, before (and apart from) the
        # venv, so it cannot link the venv's cmeel libraries.
        clone = source.index('git clone "${TOPIC_BASED_ROS2_CONTROL_REPO}"')
        assert source.index("AS ros-workspaces") < clone < source.index('python3 -m venv')
    elif name.endswith("-full"):
        assert source.index('git clone "${TOPIC_BASED_ROS2_CONTROL_REPO}"') > source.index(
            "&& python /tmp/check-roboplan-dependency-stack.py"
        )


@pytest.mark.parametrize("relative", ("scripts/ros/build-ros2-control-runtime.sh",))
@pytest.mark.parametrize("selected", ("correct", "wrong", "missing"))
def test_isaac_upstream_source_marker_is_required_before_selection(
    relative: str, selected: str, tmp_path: Path,
) -> None:
    source = (_REPO_ROOT / relative).read_text(encoding="utf-8")
    marker = tmp_path / "UPSTREAM_COMMIT"
    pin = "aa9f9ef9525a60f8becea322537dd1d6d1bf9123"
    if selected != "missing":
        marker.write_text((pin if selected == "correct" else "7ee291ab") + "\n", encoding="utf-8")
    # Execute the actual pre-source check; no ROS graph or native build starts.
    check = source.split("# Source identity only:", 1)[1].split("set +u", 1)[0]
    check = check[check.index("if [["):]
    environment = dict(os.environ)
    if relative.endswith("build-ros2-control-runtime.sh"):
        # The ROS build helper takes its selected install as TOPIC_INSTALL and an
        # optional data overlay as DATA_OVERLAY instead of a literal path.
        environment.update({"TOPIC_INSTALL": str(tmp_path), "DATA_OVERLAY": ""})
    else:
        check = check.replace("/opt/manyforge/topic_based_ros2_control/install/UPSTREAM_COMMIT", str(marker))
    completed = subprocess.run(
        ["bash", "-eu", "-c", check], env=environment, capture_output=True, text=True, check=False
    )
    assert completed.returncode == (0 if selected == "correct" else 2)
    if selected != "correct":
        assert "missing or has the wrong source pin" in completed.stderr



@pytest.mark.parametrize("relative", ("scripts/ros/build-ros2-control-runtime.sh",))
@pytest.mark.parametrize("selected", ("correct", "stale", "missing"))
def test_isaac_selected_topic_plugin_cannot_be_shadowed(
    relative: str, selected: str, tmp_path: Path,
) -> None:
    source = (_REPO_ROOT / relative).read_text(encoding="utf-8")
    check = source.split("topic_based_prefix=$(", 1)[1].split("\nfi", 1)[0]
    check = "topic_based_prefix=$(" + check + "\nfi\n"
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    ros2 = fake_bin / "ros2"
    ros2.write_text(
        '#!/bin/bash\n[[ "$*" == "pkg prefix topic_based_ros2_control" ]] || exit 9\n'
        + ('exit 1\n' if selected == "missing" else 'printf "%s\\n" "$PROBE_PREFIX"\n'),
        encoding="utf-8",
    )
    ros2.chmod(0o755)
    prefix = "/opt/manyforge/topic_based_ros2_control/install" if selected == "correct" else "/tmp/old-fork/install"
    environment = {**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}", "PROBE_PREFIX": prefix}
    if relative.endswith("build-ros2-control-runtime.sh"):
        environment["TOPIC_INSTALL"] = "/opt/manyforge/topic_based_ros2_control/install"
    completed = subprocess.run(
        ["bash", "-eu", "-c", check],
        env=environment,
        capture_output=True, text=True, check=False,
    )
    assert completed.returncode == {"correct": 0, "stale": 2, "missing": 1}[selected]
    if selected == "stale":
        assert "actual prefix=" + prefix in completed.stderr


def test_isaac_static_peer_profile_clears_stale_dds_overrides_and_is_role_free() -> None:
    topology = _REPO_ROOT / "config/ros/isaac_static_peers.env.sh"
    environment = os.environ.copy()
    for variable in (
        "ROS_DOMAIN_ID",
        "RMW_IMPLEMENTATION",
        "ROS_AUTOMATIC_DISCOVERY_RANGE",
        "ROS_STATIC_PEERS",
        "MANYFORGE_ROS_DOMAIN_ID",
        "MANYFORGE_RMW_IMPLEMENTATION",
        "MANYFORGE_ROS_AUTOMATIC_DISCOVERY_RANGE",
        "MANYFORGE_ROS_STATIC_PEERS",
    ):
        environment.pop(variable, None)
    environment.update(
        {
            "FASTRTPS_DEFAULT_PROFILES_FILE": "/stale/fastdds.xml",
            "CYCLONEDDS_URI": "file:///stale/cyclone.xml",
        }
    )
    command = (
        f"source {str(topology)!r}; "
        "printf '%s|%s|%s|%s|%s|%s' "
        '"$ROS_DOMAIN_ID" "$RMW_IMPLEMENTATION" '
        '"$ROS_AUTOMATIC_DISCOVERY_RANGE" "$ROS_STATIC_PEERS" '
        '"${FASTRTPS_DEFAULT_PROFILES_FILE-unset}" '
        '"${CYCLONEDDS_URI-unset}"'
    )

    completed = subprocess.run(
        ["/bin/bash", "--noprofile", "--norc", "-c", command],
        cwd=_REPO_ROOT,
        env=environment,
        text=True,
        capture_output=True,
        check=True,
    )

    assert completed.stdout == (
        "0|rmw_fastrtps_cpp|LOCALHOST|" "192.168.1.132;192.168.1.68|unset|unset"
    )
    source = topology.read_text(encoding="utf-8")
    for host_role in ("THOR", "ISAAC_SIM_IP", "ORIN_IP", "MANAGER_IP"):
        assert host_role not in source


@pytest.mark.parametrize(
    "suite",
    ("integration",),
)
def test_unavailable_test_suites_fail_instead_of_skipping_or_mocking(
    suite: str,
) -> None:
    completed = subprocess.run(
        ["bash", str(_TEST_ALL), suite],
        cwd=_REPO_ROOT,
        env={**os.environ, "WORKSPACE": str(_REPO_ROOT)},
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 78, suite
    assert completed.stderr.strip() == _UNAVAILABLE_MESSAGE


def test_ros_selector_builds_and_tests_the_ros_integration_graph() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    body = source.split("run_ros_tests() {", 1)[1].split("\n}\n", 1)[0]

    assert "manyforge_fail_composable_cell_runtime_unavailable" not in body
    assert '"$WORKSPACE/scripts/ros/build-ros2-control-runtime.sh"' in body
    assert '--source-dir "$WORKSPACE/scripts/ros/ros-integration" --target all' in body
    assert '--topic-install "$ROS_INTEGRATION_TOPIC_INSTALL"' in body
    assert '--data-overlay "$ROS_INTEGRATION_DATA_OVERLAY"' in body
    assert "MANYFORGE_ROS2_CONTROL_BUILD_TESTING=ON" in body
    # Keep-going follows the configured tree's generator, in the same ROS
    # environment the helper configured it with.
    assert 'set_build_keep_going_args "$build"' in body
    assert '-- "${BUILD_KEEP_GOING_ARGS[@]}"' in body
    assert body.count("source_ros_integration_environment") == 3
    for suite in (
        "JtcExecutionPort",
        "RetainedRos2ControlRuntimeHandleTest",
        "Ros2ControlKernelStopIntegrationTest",
    ):
        assert f'"{suite}"' in body
    assert "ROS_INTEGRATION_TOPIC_INSTALL=/opt/manyforge/topic_based_ros2_control/install" in source
    assert "ROS_INTEGRATION_DATA_OVERLAY=/opt/manyforge/xarm_description/install" in source
def test_bindings_selector_routes_to_the_real_core_bindings_builder() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    body = source.split("run_pybind_tests() {", 1)[1].split("run_python_tests() {", 1)[0]

    assert "ROS-free pybind11 compiler bindings" in body
    assert "MANYFORGE_BINDINGS_PREPARE_ONLY:-0" in body
    assert "MANYFORGE_BINDINGS_STAGE_ROOT:-${BUILD_DIR}-bindings-stage" in body
    assert "MANYFORGE_BINDINGS_GENERATED_ROOT:-${BUILD_DIR}-bindings-generated" in body
    assert '"$WORKSPACE/scripts/ci/run-core-bindings.sh"' in body
    assert '--build-root "$BUILD_DIR"' in body
    assert "arguments+=(--prepare-only)" in body
    assert "manyforge_fail_composable_cell_runtime_unavailable" not in body


def test_retained_bridge_suite_qualifies_protocol_without_live_bridge() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    body = source.split("run_bridge_tests() {", 1)[1].split("run_composer_backend_tests() {", 1)[0]

    assert "manyforge_assistant_bridge/tests/" in body
    assert "pytest" in body
    assert "manyforge_fail_composable_cell_runtime_unavailable" not in body


def test_deployment_package_participates_in_repository_version_guard() -> None:
    source = (_REPO_ROOT / "scripts/ci/verify-version-sync.sh").read_text(encoding="utf-8")
    assert 'check "manyforge_deployment CMakeLists.txt"' in source
    assert "manyforge_program_runner" in source


def test_ros_services_share_the_official_full_lyrical_image() -> None:
    compose = yaml.safe_load((_REPO_ROOT / "docker-compose.yaml").read_text())
    services = compose["services"]
    for name in ("ros-jazzy", "ros-jazzy-full"):
        assert services[name]["image"] == "${MANYFORGE_ROS_FULL_IMAGE:-manyforge-ros-lyrical-full}"
    assert "build" not in services["ros-jazzy"]
    assert services["ros-jazzy-full"]["build"]["dockerfile"] == "Dockerfile.ros-lyrical-full"
    cache = (_REPO_ROOT / "Dockerfile.ros-lyrical-cache").read_text()
    assert "ARG MANYFORGE_ROS_BASE=manyforge-ros-lyrical-full:eval-20261011" in cache
    assert "/opt/ros/lyrical/setup.bash" in cache
    assert "EVALUATION ONLY" not in (_REPO_ROOT / "Dockerfile.ros-lyrical-full").read_text()
