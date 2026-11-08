#!/usr/bin/env bash
# Successor for the deleted FNV/path-derived collision mapper probe.
#
# The current product performs no runtime mapper rebuild. This entrypoint runs
# the exact compiler-emitted symbol/body point-lookup workload through the
# retained-product consumer that uses it. It never configures or builds on the
# host; supply an already qualified Docker build directory.

set -euo pipefail

if [[ "${1:-}" != "--in-container" ]]; then
  echo "composable_cell_collision_lookup_probe_requires_docker: rerun with --in-container and MANYFORGE_BUILD_DIR set to an already qualified RoboPlan build" >&2
  exit 64
fi

build_dir="${MANYFORGE_BUILD_DIR:-}"
binary="${build_dir}/test_retained_product_consumer"
if [[ -z "${build_dir}" || ! -x "${binary}" ]]; then
  echo "composable_cell_collision_lookup_probe_missing_build: test_retained_product_consumer is absent from MANYFORGE_BUILD_DIR" >&2
  exit 66
fi

exec "${binary}" \
  --gtest_filter=RetainedProductCrossConsumer.CompilerEmittedSymbolAndBodyLookupPerformanceStaysBounded
