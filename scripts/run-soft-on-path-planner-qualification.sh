#!/usr/bin/env bash
# Phase 4.5G soft_on_path planner latency qualification probe.
#
# Produces one archival JSON evidence line for the side-effect-free
# replacement-trajectory planner. This does not replace live ros2/Isaac/robot
# qualification; it proves the in-tree compute path stays bounded before the
# backend dispatch and controller-acceptance windows are measured.
#
# Usage (local Docker, from the manyforge repo root):
#   ./scripts/run-soft-on-path-planner-qualification.sh
#
# Usage (inside the dev container):
#   ./scripts/run-soft-on-path-planner-qualification.sh --in-container

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IN_CONTAINER=false
if [[ "${1:-}" == "--in-container" ]]; then
  IN_CONTAINER=true
fi

run_inside_container() {
  export MANYFORGE_SOFT_ON_PATH_PLAN_P999_BUDGET_US="${MANYFORGE_SOFT_ON_PATH_PLAN_P999_BUDGET_US:-5000}"
  local workspace="${WORKSPACE:-/workspace}"
  local build_dir="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/core"
  local source_file="${workspace}/manyforge_core/test/perf/perf_soft_on_path_planner.cpp"
  local binary="${build_dir}/perf_soft_on_path_planner"
  if [[ ! -x "${binary}" ||
        "${source_file}" -nt "${binary}" ||
        "${MANYFORGE_FORCE_REBUILD_QUALIFICATION:-0}" == "1" ]]; then
    echo "perf_soft_on_path_planner not built at ${build_dir}; running CMake build" >&2
    mkdir -p "${build_dir}"
    (cd "${build_dir}" && cmake "${workspace}/manyforge_core" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=ON >&2 && make -j"$(nproc)" perf_soft_on_path_planner >&2)
  fi
  "${binary}"
}

if [[ "${IN_CONTAINER}" == "true" ]]; then
  if [[ -n "${MANYFORGE_SOFT_ON_PATH_QUAL_JSON_OUT:-}" ]]; then
    run_inside_container | tee "${MANYFORGE_SOFT_ON_PATH_QUAL_JSON_OUT}"
    exit "${PIPESTATUS[0]}"
  fi
  run_inside_container
  exit $?
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "docker not found on PATH; rerun with --in-container inside the dev container" >&2
  exit 1
fi

cd "${REPO_ROOT}"
set +e
RESULT_JSON="$(docker compose run --rm dev \
  bash -lc "./scripts/run-soft-on-path-planner-qualification.sh --in-container")"
status=$?
set -e
echo "${RESULT_JSON}"
if [[ -n "${MANYFORGE_SOFT_ON_PATH_QUAL_JSON_OUT:-}" ]]; then
  echo "${RESULT_JSON}" > "${MANYFORGE_SOFT_ON_PATH_QUAL_JSON_OUT}"
fi
exit "${status}"
