#!/usr/bin/env bash
# Repeat-under-load stability gate for the execution-kernel suite.
#
# This is the codified version of the loaded loop that exposed the 2026-07-06
# teardown/lifecycle races. It intentionally runs outside ctest so a failing
# iteration preserves the exact log/backtrace artifact.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${CORE_BUILD_DIR:-${MANYFORGE_CORE_BUILD_DIR:-/tmp/manyforge-build/core}}"
ITERATIONS="${MANYFORGE_STABILITY_ITERATIONS:-30}"
LOAD_WORKERS="${MANYFORGE_STABILITY_LOAD_WORKERS:-3}"
TIMEOUT_SECONDS="${MANYFORGE_STABILITY_TIMEOUT_SECONDS:-30}"
LOG_DIR="${MANYFORGE_STABILITY_LOG_DIR:-${TMPDIR:-/tmp}/manyforge-stability-gate-$(date +%Y%m%d-%H%M%S)}"
TEST_BINARY="${MANYFORGE_STABILITY_TEST_BINARY:-${BUILD_DIR}/test_execution_kernel}"

mkdir -p "${LOG_DIR}"

load_pids=()
cleanup() {
  for pid in "${load_pids[@]:-}"; do
    kill "${pid}" 2>/dev/null || true
  done
  for pid in "${load_pids[@]:-}"; do
    wait "${pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cmake --build "${BUILD_DIR}" --target test_execution_kernel \
    -j"${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
fi

if [[ ! -x "${TEST_BINARY}" ]]; then
  echo "test_execution_kernel not found at ${TEST_BINARY}" >&2
  echo "Set CORE_BUILD_DIR or MANYFORGE_STABILITY_TEST_BINARY." >&2
  exit 2
fi

find_libsegfault() {
  local candidate
  for candidate in \
    /lib/*/libSegFault.so \
    /usr/lib/*/libSegFault.so \
    /lib/libSegFault.so \
    /usr/lib/libSegFault.so; do
    if [[ -f "${candidate}" ]]; then
      echo "${candidate}"
      return 0
    fi
  done
  return 1
}

libsegfault="$(find_libsegfault || true)"
if [[ -n "${libsegfault}" ]]; then
  export LD_PRELOAD="${libsegfault}${LD_PRELOAD:+:${LD_PRELOAD}}"
  export SEGFAULT_SIGNALS="${SEGFAULT_SIGNALS:-abrt segv}"
else
  echo "libSegFault.so not found; continuing without preload backtraces" >&2
fi

echo "manyforge stability gate: ${ITERATIONS} iterations, ${LOAD_WORKERS} load workers"
echo "logs: ${LOG_DIR}"

for _ in $(seq 1 "${LOAD_WORKERS}"); do
  bash -c 'while :; do :; done' &
  load_pids+=("$!")
done

for iteration in $(seq 1 "${ITERATIONS}"); do
  log="${LOG_DIR}/test_execution_kernel.${iteration}.log"
  echo "[${iteration}/${ITERATIONS}] ${TEST_BINARY}"
  set +e
  timeout -s ABRT "${TIMEOUT_SECONDS}s" \
    "${TEST_BINARY}" --gtest_brief=1 >"${log}" 2>&1
  status=$?
  set -e
  if [[ "${status}" -ne 0 ]]; then
    echo "stability gate failed at iteration ${iteration} (status ${status})" >&2
    echo "failing log: ${log}" >&2
    tail -n 200 "${log}" >&2 || true
    exit "${status}"
  fi
done

echo "stability gate passed; logs retained in ${LOG_DIR}"
