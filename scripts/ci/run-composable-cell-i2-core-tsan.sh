#!/usr/bin/env bash
# Composable Cell I2 packet-owned core TSan evidence lane.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/tsan-manyforge-core}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
GTEST_TIMEOUT_SECONDS="${MANYFORGE_TSAN_GTEST_TIMEOUT_SECONDS:-300}"
TSAN_SUPPRESSIONS="${TSAN_SUPPRESSIONS:-${REPO_ROOT}/scripts/ci/tsan.supp}"
ARTIFACT_ROOT="${MANYFORGE_NATIVE_ARTIFACT_ROOT:-${REPO_ROOT}}"
INVENTORY_JSON="${MANYFORGE_NATIVE_INVENTORY_JSON:-${BUILD_DIR}/ctest-inventory.json}"
EXECUTION_INDEX_JSON="${MANYFORGE_NATIVE_EXECUTION_INDEX_JSON:-${BUILD_DIR}/execution-index.json}"
RESULT_ROOT="${MANYFORGE_NATIVE_RESULT_ROOT:-${BUILD_DIR}/native}"
LANE_ID="${MANYFORGE_NATIVE_LANE_ID:-core_tsan}"

expected_personality="00040000"
current_personality="$(cat /proc/self/personality)"
if [[ "${current_personality}" != "${expected_personality}" ]]; then
  if [[ "${MANYFORGE_TSAN_SETARCH_REEXEC:-0}" == "1" ]]; then
    echo "TSan ADDR_NO_RANDOMIZE re-exec did not take effect" >&2
    exit 2
  fi
  export MANYFORGE_TSAN_SETARCH_REEXEC=1
  exec /usr/bin/setarch x86_64 -R "$0" "$@"
fi
unset MANYFORGE_TSAN_SETARCH_REEXEC
child_personality="$(sh -c 'cat /proc/self/personality')"
if [[ "${child_personality}" != "${expected_personality}" ]]; then
  echo "TSan ADDR_NO_RANDOMIZE inheritance check failed" >&2
  exit 2
fi

if [[ ! -f "${TSAN_SUPPRESSIONS}" ]]; then
  echo "TSan suppression file not found: ${TSAN_SUPPRESSIONS}" >&2
  exit 2
fi

export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 history_size=7 exitcode=66 suppressions=${TSAN_SUPPRESSIONS}"
unset ASAN_OPTIONS UBSAN_OPTIONS 2>/dev/null || true

mkdir -p "${BUILD_DIR}/.cmake/api/v1/query"
printf '{}\n' > "${BUILD_DIR}/.cmake/api/v1/query/codemodel-v2"
printf '{}\n' > "${BUILD_DIR}/.cmake/api/v1/query/cmakeFiles-v1"
printf '%s\n' \
  'set(GRAPHVIZ_GRAPH_NAME "manyforge_i2")' \
  'set(GRAPHVIZ_GRAPH_HEADER "node [ fontsize = \"12\" ];")' \
  'set(GRAPHVIZ_NODE_PREFIX "i2node")' \
  'set(GRAPHVIZ_EXECUTABLES TRUE)' \
  'set(GRAPHVIZ_STATIC_LIBS TRUE)' \
  'set(GRAPHVIZ_SHARED_LIBS TRUE)' \
  'set(GRAPHVIZ_MODULE_LIBS TRUE)' \
  'set(GRAPHVIZ_INTERFACE_LIBS TRUE)' \
  'set(GRAPHVIZ_OBJECT_LIBS TRUE)' \
  'set(GRAPHVIZ_UNKNOWN_LIBS TRUE)' \
  'set(GRAPHVIZ_EXTERNAL_LIBS TRUE)' \
  'set(GRAPHVIZ_CUSTOM_TARGETS TRUE)' \
  'set(GRAPHVIZ_IGNORE_TARGETS "")' \
  'set(GRAPHVIZ_GENERATE_PER_TARGET FALSE)' \
  'set(GRAPHVIZ_GENERATE_DEPENDERS FALSE)' \
  > "${BUILD_DIR}/CMakeGraphVizOptions.cmake"

# `-Wno-error=array-bounds` works around a known gcc-13 false positive on
# `std::vector<T>::push_back` where the inner element is itself a small
# `std::vector<>` (here: `TrajectoryDTO` holding `TrajectoryWaypointDTO`
# holding `vector<double>`). Under TSan's `RelWithDebInfo` flags the
# optimizer's inlined memmove path triggers a spurious "forming offset N
# is out of the bounds [0, N]" warning. The warning stays visible in CI
# logs (no `-Wno-array-bounds`) but is not promoted to error.
/usr/bin/cmake \
  -G Ninja \
  --graphviz="${BUILD_DIR}/manyforge-i2-target-graph.dot" \
  -S "${REPO_ROOT}/manyforge_core" \
  -B "${BUILD_DIR}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS="${GTEST_TIMEOUT_SECONDS}" \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -Wno-error=array-bounds" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"

/usr/bin/ninja -C "${BUILD_DIR}" -j "${PARALLEL}" -d keeprsp
/usr/local/bin/python3 "${SCRIPT_DIR}/run-composable-cell-i2-native-evidence.py" \
  --lane-id "${LANE_ID}" \
  --sanitizer thread \
  --artifact-root "${ARTIFACT_ROOT}" \
  --build-dir "${BUILD_DIR}" \
  --inventory-json "${INVENTORY_JSON}" \
  --execution-index-json "${EXECUTION_INDEX_JSON}" \
  --result-root "${RESULT_ROOT}" \
  --timeout-seconds "${GTEST_TIMEOUT_SECONDS}" \
  --require-zero-disabled \
  --require-zero-skips \
  --forbid-fallback
