#!/usr/bin/env bash
# Composable Cell I2 packet-owned core ASan/UBSan evidence lane.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/asan-core}"
PARALLEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
GTEST_TIMEOUT_SECONDS="${MANYFORGE_ASAN_GTEST_TIMEOUT_SECONDS:-300}"
ARTIFACT_ROOT="${MANYFORGE_NATIVE_ARTIFACT_ROOT:-${REPO_ROOT}}"
INVENTORY_JSON="${MANYFORGE_NATIVE_INVENTORY_JSON:-${BUILD_DIR}/ctest-inventory.json}"
EXECUTION_INDEX_JSON="${MANYFORGE_NATIVE_EXECUTION_INDEX_JSON:-${BUILD_DIR}/execution-index.json}"
RESULT_ROOT="${MANYFORGE_NATIVE_RESULT_ROOT:-${BUILD_DIR}/native}"
LANE_ID="${MANYFORGE_NATIVE_LANE_ID:-core_asan_ubsan}"

export ASAN_OPTIONS="halt_on_error=1:detect_leaks=1:exitcode=66"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:exitcode=66"
unset TSAN_OPTIONS 2>/dev/null || true

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

/usr/bin/cmake \
  -G Ninja \
  --graphviz="${BUILD_DIR}/manyforge-i2-target-graph.dot" \
  -S "${REPO_ROOT}/manyforge_core" \
  -B "${BUILD_DIR}" \
  -DBUILD_TESTING=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMANYFORGE_GTEST_TIMEOUT_SECONDS="${GTEST_TIMEOUT_SECONDS}" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -Wno-error=maybe-uninitialized -Wno-error=array-bounds" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"

/usr/bin/ninja -C "${BUILD_DIR}" -j "${PARALLEL}" -d keeprsp
/usr/local/bin/python3 "${SCRIPT_DIR}/run-composable-cell-i2-native-evidence.py" \
  --lane-id "${LANE_ID}" \
  --sanitizer address+undefined \
  --artifact-root "${ARTIFACT_ROOT}" \
  --build-dir "${BUILD_DIR}" \
  --inventory-json "${INVENTORY_JSON}" \
  --execution-index-json "${EXECUTION_INDEX_JSON}" \
  --result-root "${RESULT_ROOT}" \
  --timeout-seconds "${GTEST_TIMEOUT_SECONDS}" \
  --require-zero-disabled \
  --require-zero-skips \
  --forbid-fallback
