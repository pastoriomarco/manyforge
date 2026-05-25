#!/usr/bin/env bash
# Chunk 2E qualification probe for the neutral manyforge_collision engine.
#
# Produces one Phase-4 JSON object containing the fixture matrix,
# ManyForge-vs-RoboPlan parity, serial-vs-parallel validation parity,
# fail-closed unsupported geometry evidence, and query / validation latency
# percentiles. The script is intentionally separate from ctest: unit tests
# prove correctness invariants, while this probe produces evidence that can be
# archived with qualification runs.
#
# Usage (local Docker, from the manyforge repo root):
#   ./scripts/run-collision-engine-qualification.sh
#
# Usage (inside the dev container):
#   ./scripts/run-collision-engine-qualification.sh --in-container

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IN_CONTAINER=false
if [[ "${1:-}" == "--in-container" ]]; then
  IN_CONTAINER=true
fi

source_ros_setup_if_available() {
  if [[ -f /opt/ros/jazzy/setup.bash ]]; then
    # The in-container CMake path may need ament_package from the ROS Python
    # tree even when the active Python executable comes from the ManyForge venv.
    # Source ROS explicitly so qualification does not depend on caller state.
    # shellcheck disable=SC1091
    set +u
    source /opt/ros/jazzy/setup.bash
    set -u
  fi
  local cmeel_prefix="${MANYFORGE_CMEEL_PREFIX:-/opt/manyforge/venv/lib/python3.12/site-packages/cmeel.prefix}"
  if [[ -d "${cmeel_prefix}" ]]; then
    export CMAKE_PREFIX_PATH="${cmeel_prefix}:${CMAKE_PREFIX_PATH:-}"
    export LD_LIBRARY_PATH="${cmeel_prefix}/lib:${LD_LIBRARY_PATH:-}"
    export PKG_CONFIG_PATH="${cmeel_prefix}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  fi
}

run_inside_container() {
  source_ros_setup_if_available
  local build_dir="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/roboplan"
  local source_file="${WORKSPACE:-/workspace}/tests/cpp/perf/perf_collision_engine_qualification.cpp"
  local binary="${build_dir}/manyforge_integration_tests/perf_collision_engine_qualification"
  if [[ ! -x "${binary}" ||
        "${source_file}" -nt "${binary}" ||
        "${MANYFORGE_FORCE_REBUILD_QUALIFICATION:-0}" == "1" ]]; then
    echo "perf_collision_engine_qualification not built at ${build_dir}; running CMake build" >&2
    mkdir -p "${build_dir}"
    (cd "${build_dir}" && cmake "${WORKSPACE:-/workspace}/manyforge_roboplan" \
      -DCMAKE_BUILD_TYPE=Release >&2 && make -j"$(nproc)" perf_collision_engine_qualification >&2)
  fi
  "${binary}"
}

if [[ "${IN_CONTAINER}" == "true" ]]; then
  if [[ -n "${MANYFORGE_DIRECT_ENGINE_QUAL_JSON_OUT:-}" ]]; then
    run_inside_container | tee "${MANYFORGE_DIRECT_ENGINE_QUAL_JSON_OUT}"
    exit "${PIPESTATUS[0]}"
  fi
  run_inside_container
  exit $?
else
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found on PATH; rerun with --in-container inside the dev container" >&2
    exit 1
  fi
  cd "${REPO_ROOT}"
  set +e
  RESULT_JSON="$(docker compose run --rm dev \
    bash -lc "./scripts/run-collision-engine-qualification.sh --in-container")"
  status=$?
  set -e
  echo "${RESULT_JSON}"
  if [[ -n "${MANYFORGE_DIRECT_ENGINE_QUAL_JSON_OUT:-}" ]]; then
    echo "${RESULT_JSON}" > "${MANYFORGE_DIRECT_ENGINE_QUAL_JSON_OUT}"
  fi
  exit "${status}"
fi
