"""Execute the launcher's argument seam only; no ROS graph or build."""
from pathlib import Path
import os
import subprocess

import pytest


@pytest.mark.parametrize("topic", ["", "/cell_a/arm/joint_commands"])
def test_selected_arm_topic_is_forwarded_without_an_operator_default(topic):
    source = (Path(__file__).resolve().parents[1] / "scripts/ros/launch-isaac-topic-based-control.sh").read_text()
    seam = source.split("launch_arguments=(", 1)[1].split('if [[ -n "${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY', 1)[0]
    program = 'launch_arguments=(' + seam + '\nprintf "%s\\n" "${launch_arguments[@]}"\n'
    result = subprocess.run(["bash", "-eu", "-c", program], env={**os.environ,
                             "isaac_profile": "ur10e", "runtime_namespace": "/manyforge/arm_a",
                             "ISAAC_JOINT_COMMANDS_TOPIC": topic,
                             "MANYFORGE_ROBOT_DESCRIPTION_FILE": "generated path/robot.urdf",
                             "MANYFORGE_CONTROLLERS_FILE": "generated path/controllers.yaml"},
                            capture_output=True, text=True, timeout=5, check=True)
    selected = [word for word in result.stdout.splitlines() if word.startswith("joint_commands_topic:=")]
    assert selected == ([f"joint_commands_topic:={topic}"] if topic else [])
    assert "robot_description_file:=generated path/robot.urdf" in result.stdout.splitlines()
    assert "controllers_file:=generated path/controllers.yaml" in result.stdout.splitlines()
    assert "command_gate_owner_namespace" not in result.stdout


@pytest.mark.parametrize("prefix", [None, "", "component_a/", "literal prefix/"])
def test_optional_frame_prefix_is_forwarded_verbatim_as_one_argument(prefix):
    source = (
        Path(__file__).resolve().parents[1]
        / "scripts/ros/launch-isaac-topic-based-control.sh"
    ).read_text()
    seam = source.split("launch_arguments=(", 1)[1].split(
        'if [[ -n "${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY', 1
    )[0]
    environment = {
        **os.environ, "isaac_profile": "ur10e", "runtime_namespace": "/manyforge/arm_a",
        "MANYFORGE_ROBOT_DESCRIPTION_FILE": "generated path/robot.urdf",
        "MANYFORGE_CONTROLLERS_FILE": "generated path/controllers.yaml",
    }
    environment.pop("MANYFORGE_FRAME_PREFIX", None)
    if prefix is not None:
        environment["MANYFORGE_FRAME_PREFIX"] = prefix
    result = subprocess.run(
        ["bash", "-eu", "-c", 'launch_arguments=(' + seam
         + '\nprintf "%s\\n" "${launch_arguments[@]}"\n'],
        env=environment, capture_output=True, text=True, timeout=5, check=True,
    )
    arguments = result.stdout.splitlines()
    assert [word for word in arguments if word.startswith("frame_prefix:=")] == (
        [f"frame_prefix:={prefix}"] if prefix else []
    )
    assert "robot_description_file:=generated path/robot.urdf" in arguments
    assert "controllers_file:=generated path/controllers.yaml" in arguments
    assert "runtime_namespace:=/manyforge/arm_a" in arguments


@pytest.mark.parametrize("missing", ("MANYFORGE_ROBOT_DESCRIPTION_FILE", "MANYFORGE_CONTROLLERS_FILE", None))
def test_generated_inputs_are_checked_before_build_or_ros(missing, tmp_path):
    source = (Path(__file__).resolve().parents[1] / "scripts/ros/launch-isaac-topic-based-control.sh").read_text()
    validation = source.split("# Generated artifacts are required", 1)[1].split("\n", 1)[1].split("# shellcheck source=../lib/realtime-resources.sh", 1)[0]
    for fragment in ("build-ros2-control-runtime.sh", "source \"/opt/ros/"):
        assert source.index("# Generated artifacts are required") < source.index(fragment)
    environment = dict(os.environ)
    for name in ("MANYFORGE_ROBOT_DESCRIPTION_FILE", "MANYFORGE_CONTROLLERS_FILE"):
        path = tmp_path / (name + " with spaces")
        path.write_text("explicit supplied artifact", encoding="utf-8")
        environment[name] = str(path)
    if missing:
        del environment[missing]
    result = subprocess.run(["bash", "-eu", "-c", validation], env=environment,
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == (2 if missing else 0)
    if missing:
        assert f"{missing} must name a readable generated file" in result.stderr
