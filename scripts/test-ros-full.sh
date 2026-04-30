#!/usr/bin/env bash
# Run validation checks in the full ROS-capable ManyForge image.
#
# Intended usage:
#   docker compose --profile ros-full run --rm ros-jazzy-full ./scripts/test-ros-full.sh

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
ROS_DISTRO="${ROS_DISTRO:-jazzy}"

if [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then
    # shellcheck disable=SC1090
    set +u
    source "/opt/ros/${ROS_DISTRO}/setup.bash"
    set -u
fi

if [ -n "${MANYFORGE_VENV:-}" ] && [ -f "${MANYFORGE_VENV}/bin/activate" ]; then
    # shellcheck disable=SC1091
    set +u
    source "${MANYFORGE_VENV}/bin/activate"
    set -u
fi

if [ -f "${WORKSPACE}/install/setup.bash" ]; then
    # shellcheck disable=SC1091
    set +u
    source "${WORKSPACE}/install/setup.bash"
    set -u
fi

export PYTHONPATH="${WORKSPACE}/manyforge_ros:${WORKSPACE}/manyforge_behavior:${WORKSPACE}:${PYTHONPATH:-}"

cd "$WORKSPACE"

python3 - <<'PY'
import importlib

required = [
    "fastapi",
    "httpx",
    "py_trees",
    "rclpy",
    "control_msgs",
    "trajectory_msgs",
    "manyforge_ros.inspect_isaac_scene",
    "manyforge_composer.backend.app",
]

for module in required:
    importlib.import_module(module)
    print(f"{module}: ok")
PY

pytest manyforge_ros/tests/ -q
pytest manyforge_composer/tests/test_assistant_routes.py -q
./scripts/smoke-assistant-scene-authoring.py \
  --fixture manyforge_composer/tests/fixtures/isaac_scene_authoring_soup_can.json \
  --include-subtree /World/soup_can \
  --source-root /World/soup_can \
  --object-id-prefix sim_

./scripts/smoke-assistant-scene-authoring.py \
  --transport api \
  --workflow proposal-lifecycle \
  --fixture manyforge_composer/tests/fixtures/isaac_scene_authoring_soup_can.json \
  --include-subtree /World/soup_can \
  --source-root /World/soup_can \
  --object-id-prefix sim_
