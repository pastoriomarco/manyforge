"""Build selection and repeated-configuration contracts; no production compilation."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts/ros/build-ros2-control-runtime.sh"
PIN = "aa9f9ef9525a60f8becea322537dd1d6d1bf9123"


def test_managed_ros_host_and_worker_keep_the_same_artifact_directory() -> None:
    source = (ROOT / "scripts/ros/ros-integration/CMakeLists.txt").read_text()
    targets = source.split("set_target_properties(", 1)[1].split("PROPERTIES", 1)[0].split()
    assert {"manyforge_ros2_control_execution_host", "manyforge_roboplan_candidate_worker"} <= set(targets)
    assert 'RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"' in source


@pytest.fixture
def build_fixture(tmp_path: Path) -> dict[str, str]:
    source = tmp_path / "source"
    for name in ("manyforge_roboplan/CMakeLists.txt", "manyforge_ros2_control_execution/CMakeLists.txt",
                 "manyforge_xarm_control/CMakeLists.txt", "manyforge_isaac_adapter/CMakeLists.txt",
                 "manyforge_ros_bringup/package.xml",
                 "aggregate/CMakeLists.txt"):
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    binaries = tmp_path / "bin"
    binaries.mkdir()
    stub = binaries / "stub"
    stub.write_text(
        f"#!{sys.executable}\n"
        "import json, os, sys\nfrom pathlib import Path\n"
        "tool=Path(sys.argv[0]).name; args=sys.argv[1:]\n"
        "with Path(os.environ['CALLS']).open('a') as out:\n"
        " out.write(json.dumps({'tool':tool,'args':args,'prefix':os.environ.get('CMAKE_PREFIX_PATH','')})+'\\n')\n"
        "if tool=='ros2':\n"
        " value=os.environ['TOPIC_PREFIX'] if args[-1]=='topic_based_ros2_control' else os.environ['DATA_PREFIX']\n"
        " if value=='MISSING': sys.exit(1)\n"
        " print(value)\n"
        "elif tool=='find': print(os.environ['PINOCCHIO_CONFIG'])\n"
        "elif tool=='cmake' and args[0]=='--build':\n"
        " selected=args[args.index('--target')+1:args.index('--parallel')]\n"
        " for name in ('manyforge_ros2_control_execution_host','manyforge_roboplan_candidate_worker'):\n"
        "  if name not in selected and 'all' not in selected: continue\n"
        "  p=Path(args[1])/name; p.touch(); p.chmod(0o755)\n"
        "elif tool=='colcon':\n"
        " p=Path(args[args.index('--install-base')+1]); p.mkdir(parents=True,exist_ok=True); (p/'setup.bash').touch()\n",
        encoding="utf-8",
    )
    stub.chmod(0o755)
    for name in ("ros2", "find", "cmake", "colcon"):
        (binaries / name).symlink_to(stub)
    topic = tmp_path / "topic"
    topic.mkdir()
    (topic / "UPSTREAM_COMMIT").write_text(PIN + "\n")
    (topic / "setup.bash").write_text(f'export PATH="{binaries}:$PATH"\n')
    data = tmp_path / "data"
    data.mkdir()
    (data / "setup.bash").write_text("")
    build = tmp_path / "build"
    build.mkdir()
    return {
        **os.environ, "WORKSPACE": str(source), "MANYFORGE_VENV": str(tmp_path / "no-venv"),
        "MANYFORGE_ROS2_CONTROL_BUILD_ROOT": str(build), "CMAKE_BUILD_PARALLEL_LEVEL": "2",
        "MANYFORGE_ROS2_CONTROL_BUILD_TYPE": "Release", "MANYFORGE_ROS2_CONTROL_BUILD_TESTING": "ON",
        "CALLS": str(tmp_path / "calls.jsonl"), "TOPIC_PREFIX": str(topic),
        "DATA_ROOT": str(data), "DATA_PREFIX": str(data / "xarm_description"),
        "PINOCCHIO_CONFIG": str(tmp_path / "cmeel.prefix/lib/cmake/pinocchio"),
        "CMAKE_PREFIX_PATH": f"{tmp_path}/cmeel.prefix:/qualified/ordinary",
    }


def run(env: dict[str, str], *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["bash", str(HELPER), *args], env=env, capture_output=True,
                          text=True, check=False, timeout=15)


def calls(env: dict[str, str]) -> list[dict]:
    path = Path(env["CALLS"])
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def test_help_needs_no_ros_or_build_inputs(build_fixture: dict[str, str]) -> None:
    result = run({**build_fixture, "WORKSPACE": "/missing"}, "--help")
    assert result.returncode == 0 and "--source-dir" in result.stdout
    assert calls(build_fixture) == []


@pytest.mark.parametrize("args,setting", [
    (("--target", "bad;target"), {}), (("--source-dir",), {}), (("--unknown",), {}),
    ((), {"CMAKE_BUILD_PARALLEL_LEVEL": "0"}),
    ((), {"MANYFORGE_ROS2_CONTROL_BUILD_TYPE": "unknown"}),
    ((), {"MANYFORGE_ROS2_CONTROL_BUILD_TESTING": "yes"}),
])
def test_invalid_selection_cannot_configure(build_fixture: dict[str, str], args: tuple,
                                          setting: dict[str, str]) -> None:
    assert run({**build_fixture, **setting}, *args).returncode == 2
    assert calls(build_fixture) == []


def test_default_targets_and_single_graph(build_fixture: dict[str, str]) -> None:
    result = run(build_fixture, "--topic-install", build_fixture["TOPIC_PREFIX"])
    assert result.returncode == 0, result.stderr
    commands = calls(build_fixture)
    configured = [call for call in commands if call["tool"] == "cmake" and call["args"][0] == "-S"]
    assert len(configured) == 1
    assert configured[0]["args"][1] == build_fixture["WORKSPACE"] + "/manyforge_roboplan"
    built = next(call["args"] for call in commands if call["tool"] == "cmake" and call["args"][0] == "--build")
    assert built[built.index("--target") + 1:built.index("--parallel")] == [
        "manyforge_ros2_control_execution_host", "manyforge_roboplan_candidate_worker"]
    overlay = next(call for call in commands if call["tool"] == "colcon")
    assert "cmeel.prefix" not in overlay["prefix"] and "/qualified/ordinary" in overlay["prefix"]


def test_aggregate_and_explicit_data_install(build_fixture: dict[str, str]) -> None:
    source = build_fixture["WORKSPACE"] + "/aggregate"
    result = run(build_fixture, "--topic-install", build_fixture["TOPIC_PREFIX"],
                 "--source-dir", source, "--target", "all", "--data-overlay", build_fixture["DATA_ROOT"])
    assert result.returncode == 0, result.stderr
    commands = calls(build_fixture)
    configured = next(call for call in commands if call["tool"] == "cmake" and call["args"][0] == "-S")
    assert configured["args"][1] == source and "-DBUILD_TESTING=ON" in configured["args"]
    built = next(call["args"] for call in commands if call["tool"] == "cmake" and call["args"][0] == "--build")
    assert built[built.index("--target") + 1:built.index("--parallel")] == ["all"]
    assert ["pkg", "prefix", "xarm_description"] in [call["args"] for call in commands]
    overlay = next(call["args"] for call in commands if call["tool"] == "colcon")
    assert not any("xarm_description" in argument for argument in overlay)


@pytest.mark.parametrize("kind", ["missing_marker", "wrong_marker", "wrong_topic", "wrong_data",
                                  "missing_topic", "missing_data"])
def test_wrong_selected_install_cannot_configure(build_fixture: dict[str, str], kind: str) -> None:
    topic = Path(build_fixture["TOPIC_PREFIX"])
    env = dict(build_fixture)
    if kind == "missing_marker":
        (topic / "UPSTREAM_COMMIT").unlink()
    elif kind == "wrong_marker":
        (topic / "UPSTREAM_COMMIT").write_text("wrong\n")
    elif kind == "wrong_topic":
        env["TOPIC_PREFIX"] = "/stale/topic"
    elif kind == "missing_topic":
        env["TOPIC_PREFIX"] = "MISSING"
    elif kind == "missing_data":
        env["DATA_PREFIX"] = "MISSING"
    else:
        env["DATA_PREFIX"] = "/stale/data"
    result = run(env, "--topic-install", str(topic), "--data-overlay", build_fixture["DATA_ROOT"])
    assert result.returncode == (1 if kind in {"missing_topic", "missing_data"} else 2)
    assert not any(call["tool"] in {"cmake", "colcon"} for call in calls(env))


def test_repeated_targets_replace_default_list(build_fixture: dict[str, str]) -> None:
    result = run(build_fixture, "--topic-install", build_fixture["TOPIC_PREFIX"],
                 "--target", "test_ros2_control_execution_backend",
                 "--target", "test_ros2_gripper_command_backend")
    assert result.returncode == 0, result.stderr
    built = next(call["args"] for call in calls(build_fixture)
                 if call["tool"] == "cmake" and call["args"][0] == "--build")
    assert built[built.index("--target") + 1:built.index("--parallel")] == [
        "test_ros2_control_execution_backend", "test_ros2_gripper_command_backend"]


@pytest.mark.parametrize("args", [("--source-dir", "/missing"), ("--data-overlay", "/missing")])
def test_missing_input_cannot_configure(build_fixture: dict[str, str], args: tuple) -> None:
    assert run(build_fixture, "--topic-install", build_fixture["TOPIC_PREFIX"], *args).returncode == 2
    assert calls(build_fixture) == []


@pytest.mark.parametrize("shared", [None, "OFF"])
def test_graph_library_types_survive_identical_reconfigure(
    tmp_path: Path, shared: str | None,
) -> None:
    # Exercise the real integration owner with small package targets. The adapter
    # introduces the same late default as ament_cmake_ros_core, after Core exists.
    source = tmp_path / "source"
    graph = source / "scripts/ros/ros-integration"
    graph.mkdir(parents=True)
    (graph / "CMakeLists.txt").write_text(
        (ROOT / "scripts/ros/ros-integration/CMakeLists.txt").read_text())
    targets = []
    for package in ("manyforge_core", "manyforge_deployment",
                    "manyforge_ros2_control_execution", "manyforge_xarm_control",
                    "manyforge_roboplan"):
        directory = source / package
        directory.mkdir()
        (directory / "source.cpp").write_text("int main() { return 0; }\n")
        cmake = ""
        if package == "manyforge_ros2_control_execution":
            cmake += 'option(BUILD_SHARED_LIBS "ROS dependency default" ON)\n'
        cmake += f"add_library({package} source.cpp)\n"
        if package == "manyforge_roboplan":
            for target in ("manyforge_ros2_control_execution_host",
                           "manyforge_roboplan_candidate_worker"):
                cmake += f"add_executable({target} source.cpp)\n"
        (directory / "CMakeLists.txt").write_text(cmake)
        if package == "manyforge_ros2_control_execution":
            # The report-only target is excluded from ALL, but CMake still
            # requires its source to exist while configuring this small graph.
            perf = directory / "test/perf/perf_ros2_control_hot_paths.cpp"
            perf.parent.mkdir(parents=True)
            perf.write_text("int main() { return 0; }\n")
        targets.append(package)
    with (graph / "CMakeLists.txt").open("a") as out:
        output = "".join(f"{target}=$<TARGET_PROPERTY:{target},TYPE>\n"
                         for target in targets)
        out.write('\nfile(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/types.txt" CONTENT "'
                  + output + '")\n')
    build = tmp_path / "build"
    command = ["cmake", "-S", str(graph), "-B", str(build)]
    if shared is not None:
        command.append(f"-DBUILD_SHARED_LIBS={shared}")
    snapshots = []
    for _ in range(2):
        configured = subprocess.run(command, capture_output=True, text=True,
                                    check=False, timeout=30)
        assert configured.returncode == 0, configured.stdout + configured.stderr
        snapshots.append((
            (build / "types.txt").read_text(),
            next(line for line in (build / "CMakeCache.txt").read_text().splitlines()
                 if line.startswith("BUILD_SHARED_LIBS:")),
        ))
    assert snapshots[0] == snapshots[1]
    expected_type = "STATIC_LIBRARY" if shared == "OFF" else "SHARED_LIBRARY"
    assert snapshots[0][0].splitlines() == [
        f"{target}={expected_type}" for target in targets]
    assert snapshots[0][1] == f"BUILD_SHARED_LIBS:BOOL={shared or 'ON'}"
