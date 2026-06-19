#!/usr/bin/env bash
# Offline controlled-stop qualification.
#
# This script verifies deterministic, non-live controlled-stop surfaces:
# policy gates, UI selectors, core planner/kernel tests, native backend parity,
# ros2_control capability evidence, and planner latency. It does not replace
# Isaac or physical robot validation.
#
# Host usage:
#   ./scripts/qualify-controlled-stop-offline.sh
#
# Container usage:
#   ./scripts/qualify-controlled-stop-offline.sh --in-container

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTAINER="${MANYFORGE_QUAL_CONTAINER:-manyforge-composer-ros-1}"
IN_CONTAINER=false
if [[ "${1:-}" == "--in-container" ]]; then
  IN_CONTAINER=true
fi

run_step() {
  echo
  echo "==> $*"
  "$@"
}

run_in_container() {
  local workspace="${WORKSPACE:-/workspace}"
  cd "${workspace}"

  export PYTHONPATH="${workspace}/manyforge_behavior:${workspace}/manyforge_composer:${workspace}/manyforge_core/python:${workspace}/manyforge_roboplan/python:${PYTHONPATH:-}"

  run_step pytest -q \
    manyforge_composer/tests/test_backend.py \
    manyforge_composer/tests/test_event_recorder.py

  run_step bash -lc \
    "cd manyforge_composer/frontend && npm test -- src/store/stopProfile.test.ts src/components/DeploymentPanel.robotControlLimits.test.ts src/components/KernelErrorPanel.test.ts"
  run_step bash -lc "cd manyforge_composer/frontend && npm run build"

  local core_build="${MANYFORGE_CORE_BUILD_DIR:-/tmp/manyforge-build/core-controlled-stop-offline}"
  run_step cmake -S "${workspace}/manyforge_core" -B "${core_build}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON
  run_step cmake --build "${core_build}" -j"$(nproc)" --target \
    test_soft_on_path_stop_planner \
    test_interrupt_manager \
    test_execution_kernel \
    perf_soft_on_path_planner
  run_step "${core_build}/test_soft_on_path_stop_planner"
  run_step "${core_build}/test_interrupt_manager"
  run_step "${core_build}/test_execution_kernel"
  run_step "${core_build}/perf_soft_on_path_planner"

  local ros_distro="${ROS_DISTRO:-jazzy}"
  set +u
  source "/opt/ros/${ros_distro}/setup.bash"
  source "/opt/manyforge/topic_based_ros2_control/install/setup.bash" 2>/dev/null || true
  if [[ -d "${MANYFORGE_VENV:-/opt/manyforge/venv}" ]]; then
    source "${MANYFORGE_VENV:-/opt/manyforge/venv}/bin/activate"
  fi
  set -u

  local pinocchio_cmake_dir
  pinocchio_cmake_dir=$(
    find "${MANYFORGE_VENV:-/opt/manyforge/venv}" /usr \
      -name "pinocchioConfig.cmake" -printf '%h\n' 2>/dev/null | head -1 || true
  )
  if [[ -z "${pinocchio_cmake_dir}" ]]; then
    echo "pinocchioConfig.cmake not found; cannot build roboplan stop tests." >&2
    exit 1
  fi
  local pinocchio_prefix
  pinocchio_prefix=$(dirname "$(dirname "$(dirname "${pinocchio_cmake_dir}")")")
  export CMAKE_PREFIX_PATH="/usr/local:${pinocchio_prefix}:${CMAKE_PREFIX_PATH:-}"

  local pybind11_dir
  pybind11_dir=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())")
  local roboplan_build="${MANYFORGE_ROBOPLAN_BUILD_DIR:-/tmp/manyforge-build/roboplan-controlled-stop-offline}"
  run_step cmake -S "${workspace}/manyforge_roboplan" -B "${roboplan_build}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMANYFORGE_ENABLE_ROS2_CONTROL=ON \
    -DBUILD_TESTING=ON \
    -Dpybind11_DIR="${pybind11_dir}"
  run_step cmake --build "${roboplan_build}" -j"$(nproc)" --target \
    manyforge_roboplan_py \
    test_native_backends \
    test_ros2_capability
  run_step "${roboplan_build}/test_native_backends"
  run_step "${roboplan_build}/test_ros2_capability"

  echo
  echo "Offline controlled-stop qualification passed."
}

if [[ "${IN_CONTAINER}" == "true" ]]; then
  run_in_container
  exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "docker not found on PATH; rerun with --in-container inside the dev container." >&2
  exit 1
fi

if ! docker ps --format '{{.Names}}' | grep -qx "${CONTAINER}"; then
  echo "Container ${CONTAINER} is not running. Start the ManyForge Docker lane first." >&2
  exit 1
fi

cd "${REPO_ROOT}"
docker exec "${CONTAINER}" bash -lc "cd /workspace && ./scripts/qualify-controlled-stop-offline.sh --in-container"
