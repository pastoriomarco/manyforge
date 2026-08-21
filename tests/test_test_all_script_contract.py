"""Cutover-boundary contracts for repository entry points and test lanes."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
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
_LAUNCH_UNAVAILABLE_MESSAGE = (
    "composable_cell_runtime_unavailable: this legacy aggregate launcher is not "
    "a current product entrypoint; maintained ManyForge-only runtime functions "
    "use the Component Manager, Headless, Program runner, and Composer "
    "entrypoints, while unified Isaac, real-robot, and owner-issued cleanup "
    "orchestration remain at their later gates."
)
_BENCHMARK_UNAVAILABLE_MESSAGE = (
    "composable_cell_runtime_unavailable: the addressed native benchmark "
    "requires the build-qualified N7 process fixture; run its RoboPlan process "
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
    assert "tests/test_qualification_wrapper_entrypoints.py" in source
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

    assert source.index('if __name__ == "__main__":') < source.index(
        "from manyforge_behavior"
    )
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
    body = source.split("run_roboplan_tests() {", 1)[1].split(
        "run_headless_cpp_tests() {", 1
    )[0]

    assert "C++ retained RoboPlan product-consumer tests" in body
    assert "MANYFORGE_ENABLE_ROS2_CONTROL" not in body
    assert "manyforge_roboplan_py" not in body
    assert "manyforge_roboplan/python" not in body
    assert "configure_roboplan_build" in body
    assert body.count("ctest --output-on-failure") == 1


def test_headless_selector_builds_producer_closure_before_native_headless(
    tmp_path: Path,
) -> None:
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    build_root = tmp_path / "build"
    marker = tmp_path / "events"

    (fake_bin / "cmake").write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "if [ \"${1:-}\" = \"--build\" ]; then\n"
        "  root=\"$2\"\n"
        "  if [ \"$root\" = \"${MANYFORGE_BUILD_DIR}/roboplan\" ]; then\n"
        "    printf '%s\\n' producer-build >> \"$FAKE_LOG\"\n"
        "    printf 'producer-targets:%s\\n' \"$*\" >> \"$FAKE_LOG\"\n"
        "    mkdir -p \"$root/qualification/native_stop\" \\\n"
        "      \"$root/qualification/native_stop_o1_4g_replacement\"\n"
        "    for output in \\\n"
        "      manyforge_native_execution_host \\\n"
        "      manyforge_native_execution_host_pick_place_fixture \\\n"
        "      manyforge_roboplan_candidate_worker \\\n"
        "      manyforge_native_stop_qualification_runner; do\n"
        "      : > \"$root/$output\"\n"
        "    done\n"
        "    for output in \\\n"
        "      native_stop/implementation-build.json \\\n"
        "      native_stop/runner-contract.json \\\n"
        "      native_stop/implementation-evidence.json \\\n"
        "      native_stop/substitution-evidence.json \\\n"
        "      native_stop_o1_4g_replacement/implementation-build.json \\\n"
        "      native_stop_o1_4g_replacement/runner-contract.json \\\n"
        "      native_stop_o1_4g_replacement/implementation-evidence.json \\\n"
        "      native_stop_o1_4g_replacement/substitution-evidence.json; do\n"
        "      : > \"$root/qualification/$output\"\n"
        "    done\n"
        "  elif [ \"$root\" = \"${MANYFORGE_BUILD_DIR}/headless\" ]; then\n"
        "    printf '%s\\n' headless-build >> \"$FAKE_LOG\"\n"
        "    mkdir -p \"$root/manager-apply-test-fixture\" \\\n"
        "      \"$root/manager-apply-o1-4g-replacement-fixture\"\n"
        "    : > \"$root/manager-apply-test-fixture/metadata.json\"\n"
        "    : > \"$root/manager-apply-o1-4g-replacement-fixture/metadata.json\"\n"
        "  fi\n"
        "  exit 0\n"
        "fi\n"
        "case \"$1\" in\n"
        "  */manyforge_roboplan)\n"
        "    printf '%s\\n' producer-configure >> \"$FAKE_LOG\"\n"
        "    printf 'producer-args:%s\\n' \"$*\" >> \"$FAKE_LOG\";;\n"
        "  */manyforge_headless)\n"
        "    printf '%s\\n' headless-configure >> \"$FAKE_LOG\"\n"
        "    printf 'headless-args:%s\\n' \"$*\" >> \"$FAKE_LOG\";;\n"
        "esac\n",
        encoding="utf-8",
    )
    (fake_bin / "cmake").chmod(0o755)
    (fake_bin / "ctest").write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "printf 'ctest:%s\\n' \"$*\" >> \"$FAKE_LOG\"\n"
        "if [ \"${1:-}\" = \"-N\" ]; then\n"
        "  printf '%s\\n' 'Test #1: ManagerDaemonProcess.RejectsPreManifestRootWithTypedStartupErrorBeforeIngress'\n"
        "  if [ \"${FAKE_CTEST_OMIT_NATIVE:-0}\" != 1 ]; then\n"
        "    printf '%s\\n' \\\n"
        "      'Test #2: ManagerDaemonProcess.DistinctNativePackageUsesFreshManagerGenerationAfterCleanStop' \\\n"
        "      'Test #3: ManagerDaemonProcess.RuntimeOnlyNamedApplyUsesTheRealManagedFenceAndSurvivesReplacement' \\\n"
        "      'Test #4: ManagerDaemonProcess.PhysicalCompositionReopensAfterChildAndManagerReplacement'\n"
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
        "WORKSPACE": str(_REPO_ROOT),
    }
    completed = subprocess.run(
        ["bash", str(_TEST_ALL), "headless"],
        cwd=_REPO_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0
    events = marker.read_text(encoding="utf-8").splitlines()
    assert events.index("producer-configure") < events.index("producer-build")
    assert events.index("producer-build") < events.index("headless-configure")
    assert events.index("headless-configure") < events.index("headless-build")
    producer_targets = next(
        event for event in events if event.startswith("producer-targets:")
    )
    assert "manyforge_run_native_stop_qualification" in producer_targets
    assert "manyforge_run_native_stop_o1_4g_replacement_qualification" in producer_targets
    assert "manyforge_roboplan_candidate_worker" in producer_targets
    headless_args = next(
        event for event in events if event.startswith("headless-args:")
    )
    assert (
        "-DMANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST="
        f"{build_root}/roboplan/manyforge_native_execution_host"
    ) in headless_args
    assert sum(event.startswith("ctest:--output-on-failure") for event in events) == 1

    omitted = subprocess.run(
        ["bash", str(_TEST_ALL), "headless"],
        cwd=_REPO_ROOT,
        env={**environment, "FAKE_CTEST_OMIT_NATIVE": "1"},
        capture_output=True,
        text=True,
        check=False,
    )
    assert omitted.returncode != 0


def test_compose_retains_explicit_ros_runtime_products_and_build_cache() -> None:
    compose = yaml.safe_load(
        (_REPO_ROOT / "docker-compose.yaml").read_text(encoding="utf-8")
    )
    services = compose["services"]

    for name in ("dev", "ros-jazzy", "ros-jazzy-full", "composer", "composer-api"):
        assert services[name]["volumes"] == [".:/workspace"]
    assert services["composer-ros"]["restart"] == "no"
    assert services["composer-ros"]["command"][-1] == (
        "./scripts/ros/start-composer-ros.sh"
    )
    assert services["isaac-topic-control"]["restart"] == "no"
    assert services["isaac-topic-control"]["command"][-1] == (
        "./scripts/ros/launch-isaac-topic-based-control.sh"
    )
    assert services["isaac-topic-control"]["network_mode"] == "host"
    assert services["isaac-topic-control"]["ipc"] == "host"
    assert {
        "ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-0}",
        "RMW_IMPLEMENTATION=${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}",
        "ROS_AUTOMATIC_DISCOVERY_RANGE=${ROS_AUTOMATIC_DISCOVERY_RANGE:-LOCALHOST}",
        "ROS_STATIC_PEERS=${ROS_STATIC_PEERS:-192.168.1.132;192.168.1.68}",
    } <= set(services["isaac-topic-control"]["environment"])
    assert "ros-full-build-cache" in compose["volumes"]


def test_current_build_lanes_use_fresh_container_local_products() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    local_ci = (_REPO_ROOT / ".github/workflows/local-ci.yml").read_text(
        encoding="utf-8"
    )

    assert "MANYFORGE_TEST_STAMP" not in source
    assert ".manyforge-tested-" not in source
    assert "rm -rf" not in source
    assert 'BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"' in source
    assert 'cmake --build "$build" --parallel "$BUILD_PARALLEL_LEVEL"' in source
    assert '"${image_id}" \\' in local_ci
    assert 'cache_volume="manyforge-local-ci-build-' not in local_ci


def test_product_launcher_gates_operational_verbs_before_external_effect(
    tmp_path: Path,
) -> None:
    launcher = _REPO_ROOT / "scripts" / "launch.sh"
    source = launcher.read_text(encoding="utf-8")

    assert "manyforge_roboplan/python" not in source
    assert "stop_assistant_stack" not in source
    assert "pkill" not in source
    assert "scripts/lib/assistant.sh" not in source

    marker = tmp_path / "external-effect"
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    for command in (
        "docker",
        "curl",
        "ros2",
        "dirname",
        "realpath",
        "stat",
        "id",
        "kill",
        "sleep",
        "python3",
    ):
        stub = fake_bin / command
        stub.write_text(
            f"#!/bin/sh\nprintf '%s\\n' called >> {str(marker)!r}\n",
            encoding="utf-8",
        )
        stub.chmod(0o755)

    invocations = (
        ("start",),
        ("status",),
        ("restart",),
        ("smoke",),
        ("preflight",),
        ("stop",),
        ("--help",),
        ("start", "--lane", "not-a-lane"),
        ("stop", "--help"),
    )
    for invocation in invocations:
        completed = subprocess.run(
            ["bash", str(launcher), *invocation],
            cwd=_REPO_ROOT,
            env={**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}"},
            capture_output=True,
            text=True,
            check=False,
        )
        assert completed.returncode == 78, invocation
        assert completed.stderr.strip() == _LAUNCH_UNAVAILABLE_MESSAGE
        assert not marker.exists(), invocation


def test_product_launcher_maps_removed_cleanup_without_dormant_effects() -> None:
    launcher = _REPO_ROOT / "scripts" / "launch.sh"
    source = launcher.read_text(encoding="utf-8")

    assert "f11f1344ae2fc8b8d3875318f8d60aef7a1b6766" in source
    assert "owner-issued process/container identities at I4/I5/I9/I10" in source
    assert "verb_stop" not in source
    assert "docker " not in source
    assert "kill " not in source
    assert "rm " not in source


def test_assistant_supervisor_wrapper_is_only_a_typed_gate() -> None:
    wrapper = _REPO_ROOT / "scripts" / "run-assistant-supervisor.sh"
    source = wrapper.read_text(encoding="utf-8")

    assert "3daf0d53a35ba250d825dc45d66d3a575c04f884" in source
    assert "manyforge_fail_composable_cell_runtime_unavailable" in source
    assert "manyforge_assistant_bridge.supervisor" not in source
    assert "VENV_PYTHON" not in source
    assert re.search(r"^exec\s", source, re.MULTILINE) is None


def test_hermes_gateway_matrix_cannot_activate_the_deleted_bridge() -> None:
    matrix_path = (
        _REPO_ROOT / "scripts" / "hermes" / "managed-tool-gateway-matrix.json"
    )
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
    ("relative", "historical_blob", "entrypoint", "forbidden"),
    (
        (
            "scripts/lib/composer.sh",
            "40cb4124eee7d998ad18e820b4a369208a52a411",
            "load_program",
            ("curl", "/api/"),
        ),
        (
            "scripts/lib/compose.sh",
            "4d7e0ecc6a1be6aabd123417e03141f52da6755a",
            "start_composer_dev_container",
            ("docker run", "python -m manyforge_composer"),
        ),
        (
            "scripts/lib/isaac.sh",
            "cfe20a23a5a26d13f09cff879c11bb0ffc718c65",
            "start_isaac_services",
            ("ros2 ", "docker "),
        ),
        (
            "scripts/lib/real_robot.sh",
            "62da0b3f83d60750195e7d2154732a46f34262f2",
            "start_real_robot_lane",
            ("docker ", "/api/runtime"),
        ),
    ),
)
def test_launcher_libraries_have_no_dormant_runtime_body(
    relative: str,
    historical_blob: str,
    entrypoint: str,
    forbidden: tuple[str, ...],
    tmp_path: Path,
) -> None:
    path = _REPO_ROOT / relative
    source = path.read_text(encoding="utf-8")
    marker = tmp_path / "external-effect"
    completed = subprocess.run(
        [
            "bash",
            "-c",
            (
                f"docker() {{ printf 'called\\n' >> {str(marker)!r}; }}\n"
                f"source {str(path)!r}\n"
                f"{entrypoint}\n"
            ),
        ],
        cwd=_REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )

    assert historical_blob in source
    assert not any(token in source for token in forbidden)
    assert completed.returncode == 78
    assert "composable_cell_runtime_unavailable" in completed.stderr
    assert not marker.exists()


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


def test_i9_ros2_control_builder_selects_only_current_runtime_products() -> None:
    source = (
        _REPO_ROOT / "scripts/ros/build-ros2-control-runtime.sh"
    ).read_text(encoding="utf-8")

    assert "manyforge_fail_composable_cell_runtime_unavailable" not in source
    assert "MANYFORGE_BUILD_ROS2_CONTROL_RUNTIME=ON" in source
    assert "manyforge_ros2_control_execution_host" in source
    assert "manyforge_roboplan_candidate_worker" in source
    assert "manyforge_cclc_msgs" in source
    assert "manyforge_ros_bringup" in source
    assert "--symlink-install" in source
    for forbidden in (
        "isaac_demo_supervisor",
        "manyforge_native_execution_host",
        "composer",
        "docker ",
    ):
        assert forbidden not in source


def test_i9_isaac_launcher_enforces_the_recorded_topology_and_owner_split() -> None:
    source = (
        _REPO_ROOT / "scripts/ros/launch-isaac-topic-based-control.sh"
    ).read_text(encoding="utf-8")
    topology_source = (
        _REPO_ROOT / "config/ros/isaac_i9.env.sh"
    ).read_text(encoding="utf-8")

    assert "manyforge_fail_composable_cell_runtime_unavailable" not in source
    assert 'source "${WORKSPACE}/config/ros/isaac_i9.env.sh"' in source
    assert 'ROS_DOMAIN_ID:-0' in topology_source
    assert 'RMW_IMPLEMENTATION:-rmw_fastrtps_cpp' in topology_source
    assert (
        'ROS_AUTOMATIC_DISCOVERY_RANGE:-LOCALHOST' in topology_source
    )
    assert (
        'ROS_STATIC_PEERS:-192.168.1.132;192.168.1.68' in topology_source
    )
    for stale_override in (
        "FASTRTPS_DEFAULT_PROFILES_FILE",
        "FASTDDS_DEFAULT_PROFILES_FILE",
        "ROS_DISCOVERY_SERVER",
        "ROS_SUPER_CLIENT",
        "CYCLONEDDS_URI",
    ):
        assert stale_override in topology_source
    assert "build-ros2-control-runtime.sh" in source
    assert "isaac_topic_based_control.launch.py" in source
    assert "simulation_interfaces" in source
    assert 'runtime_namespace:=${runtime_namespace}' in source
    assert 'command_gate_owner_namespace:=${runtime_namespace}' in source
    assert "MANYFORGE_ISAAC_COMPONENT_ID" in source
    for forbidden in (
        "isaac_demo_supervisor",
        "program_path:=",
        "deployment_path:=",
        "composer_url:=",
        "docker ",
    ):
        assert forbidden not in source


def test_i9_topology_input_clears_stale_dds_overrides_and_is_role_free() -> None:
    topology = _REPO_ROOT / "config/ros/isaac_i9.env.sh"
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
        "0|rmw_fastrtps_cpp|LOCALHOST|"
        "192.168.1.132;192.168.1.68|unset|unset"
    )
    source = topology.read_text(encoding="utf-8")
    for host_role in ("THOR", "ISAAC_SIM_IP", "ORIN_IP", "MANAGER_IP"):
        assert host_role not in source


@pytest.mark.parametrize(
    "suite",
    (
        "integration",
        "qualification",
        "qualification-all",
        "qualification-native",
        "qualification-soak",
        "ros",
    ),
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


def test_bindings_selector_routes_to_the_real_i2_builder() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    body = source.split("run_pybind_tests() {", 1)[1].split(
        "run_python_tests() {", 1
    )[0]

    assert "ROS-free pybind11 compiler bindings" in body
    assert 'MANYFORGE_BINDINGS_PREPARE_ONLY:-0' in body
    assert 'MANYFORGE_BINDINGS_STAGE_ROOT:-${BUILD_DIR}-bindings-stage' in body
    assert (
        'MANYFORGE_BINDINGS_GENERATED_ROOT:-${BUILD_DIR}-bindings-generated'
        in body
    )
    assert '"$WORKSPACE/scripts/ci/run-core-bindings.sh"' in body
    assert '--build-root "$BUILD_DIR"' in body
    assert "arguments+=(--prepare-only)" in body
    assert "manyforge_fail_composable_cell_runtime_unavailable" not in body


def test_retained_bridge_suite_qualifies_protocol_without_live_bridge() -> None:
    source = _TEST_ALL.read_text(encoding="utf-8")
    body = source.split("run_bridge_tests() {", 1)[1].split(
        "run_composer_backend_tests() {", 1
    )[0]

    assert "manyforge_assistant_bridge/tests/" in body
    assert "pytest" in body
    assert "manyforge_fail_composable_cell_runtime_unavailable" not in body


def test_deployment_package_participates_in_repository_version_guard() -> None:
    source = (_REPO_ROOT / "scripts/ci/verify-version-sync.sh").read_text(
        encoding="utf-8"
    )
    assert 'check "manyforge_deployment CMakeLists.txt"' in source
    assert "manyforge_program_runner" in source
