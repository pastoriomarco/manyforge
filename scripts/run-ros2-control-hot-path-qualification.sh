#!/usr/bin/env bash
# WS8 ros2_control backend hot-path qualification probe.
#
# Produces one archival JSON evidence line for high-rate backend surfaces:
# action-feedback sample storage reuse and idle-scene repeated-sample skips.
# This does not replace live controller qualification; it proves the in-tree
# callback hot paths stay allocation-bounded after the WS8 optimizations.
#
# Usage (local Docker, from the manyforge repo root):
#   ./scripts/run-ros2-control-hot-path-qualification.sh
#
# Usage (inside the dev container):
#   ./scripts/run-ros2-control-hot-path-qualification.sh --in-container

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IN_CONTAINER=false
if [[ "${1:-}" == "--in-container" ]]; then
  IN_CONTAINER=true
fi

select_container() {
  if [[ -n "${MANYFORGE_QUAL_CONTAINER:-}" ]]; then
    echo "${MANYFORGE_QUAL_CONTAINER}"
    return 0
  fi
  docker ps --format '{{.Names}}' |
    awk '/^manyforge-(composer-ros|.*ros|isaac-topic-control)-[0-9]+$/ {print; exit}'
}

source_ros_setup_if_available() {
  local ros_distro="${ROS_DISTRO:-jazzy}"
  if [[ -f "/opt/ros/${ros_distro}/setup.bash" ]]; then
    set +u
    # shellcheck disable=SC1090
    source "/opt/ros/${ros_distro}/setup.bash"
    set -u
  fi
}

run_inside_container() {
  source_ros_setup_if_available
  export MANYFORGE_ROS2_CONTROL_FEEDBACK_ALLOC_BUDGET="${MANYFORGE_ROS2_CONTROL_FEEDBACK_ALLOC_BUDGET:-0}"
  export MANYFORGE_ROS2_CONTROL_IDLE_SKIP_ALLOC_BUDGET="${MANYFORGE_ROS2_CONTROL_IDLE_SKIP_ALLOC_BUDGET:-0}"
  export MANYFORGE_ROS2_CONTROL_IDLE_SKIP_MAX_UPDATES="${MANYFORGE_ROS2_CONTROL_IDLE_SKIP_MAX_UPDATES:-1}"

  local workspace="${WORKSPACE:-/workspace}"
  local build_dir="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/ros2-control-hot-paths"
  local source_file="${workspace}/manyforge_ros2_control_execution/test/perf/perf_ros2_control_hot_paths.cpp"
  local backend_source="${workspace}/manyforge_ros2_control_execution/src/ros2_control_execution_backend.cpp"
  local backend_header="${workspace}/manyforge_ros2_control_execution/include/manyforge_ros2_control_execution/ros2_control_execution_backend.hpp"
  local cmake_file="${workspace}/manyforge_ros2_control_execution/CMakeLists.txt"
  local binary="${build_dir}/perf_ros2_control_hot_paths"

  if [[ ! -x "${binary}" ||
        "${source_file}" -nt "${binary}" ||
        "${backend_source}" -nt "${binary}" ||
        "${backend_header}" -nt "${binary}" ||
        "${cmake_file}" -nt "${binary}" ||
        "${MANYFORGE_FORCE_REBUILD_QUALIFICATION:-0}" == "1" ]]; then
    echo "perf_ros2_control_hot_paths not built at ${build_dir}; running CMake build" >&2
    mkdir -p "${build_dir}"
    (cd "${build_dir}" && cmake "${workspace}/manyforge_ros2_control_execution" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=ON >&2 && make -j"$(nproc)" perf_ros2_control_hot_paths >&2)
  fi
  "${binary}"
}

if [[ "${IN_CONTAINER}" == "true" ]]; then
  if [[ -n "${MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT:-}" ]]; then
    run_inside_container | tee "${MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT}"
    exit "${PIPESTATUS[0]}"
  fi
  run_inside_container
  exit $?
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "docker not found on PATH; rerun with --in-container inside the dev container" >&2
  exit 1
fi

CONTAINER="$(select_container || true)"
if [[ -n "${CONTAINER}" ]] &&
   docker ps --format '{{.Names}}' | grep -qx "${CONTAINER}"; then
  cd "${REPO_ROOT}"
  docker exec "${CONTAINER}" bash -lc \
    "cd /workspace && ./scripts/run-ros2-control-hot-path-qualification.sh --in-container"
  exit $?
fi

cd "${REPO_ROOT}"
set +e
RESULT_JSON="$(docker compose run --rm dev \
  bash -lc "./scripts/run-ros2-control-hot-path-qualification.sh --in-container")"
status=$?
set -e
echo "${RESULT_JSON}"
if [[ -n "${MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT:-}" ]]; then
  echo "${RESULT_JSON}" > "${MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT}"
fi
exit "${status}"
