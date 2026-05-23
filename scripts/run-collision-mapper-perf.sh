#!/usr/bin/env bash
# Chunk 1 informational mapper perf probe for the collision-ownership plan.
#
# Runs perf_collision_mapper inside the manyforge dev container and compares
# its output against the checked-in baseline. Designed for both local
# verification and CI invocation. Phase 1 behavior: the comparison is
# informational; this script always exits 0 unless the binary itself fails.
# A future Phase 2 will flip the gate when historical CI data calibrates a
# sensible regression threshold.
#
# Usage (local Docker, from the manyforge repo root):
#   ./scripts/run-collision-mapper-perf.sh
#
# Usage (CI / inside the dev container):
#   ./scripts/run-collision-mapper-perf.sh --in-container
#
# Output: a JSON measurement plus a comparison summary against the baseline.
# When MANYFORGE_PERF_JSON_OUT is set, the JSON is also written to that path
# for further processing by CI.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE_PATH="${REPO_ROOT}/manyforge_roboplan/test/perf/collision_mapper_baseline.json"

IN_CONTAINER=false
if [[ "${1:-}" == "--in-container" ]]; then
  IN_CONTAINER=true
fi

run_inside_container() {
  local build_dir="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/roboplan"
  if [[ ! -x "${build_dir}/perf_collision_mapper" ]]; then
    echo "perf_collision_mapper not built at ${build_dir}; running CMake build" >&2
    mkdir -p "${build_dir}"
    (cd "${build_dir}" && cmake "${WORKSPACE:-/workspace}/manyforge_roboplan" \
      -DCMAKE_BUILD_TYPE=Release >&2 && make -j"$(nproc)" perf_collision_mapper >&2)
  fi
  "${build_dir}/perf_collision_mapper"
}

if [[ "${IN_CONTAINER}" == "true" ]]; then
  PERF_JSON="$(run_inside_container)"
else
  # Drive the dev container.
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found on PATH; rerun with --in-container inside the dev container" >&2
    exit 1
  fi
  PERF_JSON="$(cd "${REPO_ROOT}" && docker compose exec -T composer-ros \
    bash -lc "/workspace/scripts/run-collision-mapper-perf.sh --in-container")"
fi

echo "Measurement: ${PERF_JSON}"

if [[ -n "${MANYFORGE_PERF_JSON_OUT:-}" ]]; then
  echo "${PERF_JSON}" > "${MANYFORGE_PERF_JSON_OUT}"
fi

if [[ ! -f "${BASELINE_PATH}" ]]; then
  echo "No baseline at ${BASELINE_PATH}; first run. Treating current measurement as new baseline (commit it to lock in)."
  exit 0
fi

# Phase 1: parse + report comparison without failing.
python3 - "${BASELINE_PATH}" "${PERF_JSON}" <<'PY'
import json
import sys

baseline_path = sys.argv[1]
measurement_json = sys.argv[2]

with open(baseline_path) as f:
    baseline = json.load(f)
measurement = json.loads(measurement_json)

def pct_delta(b, m):
    if b == 0:
        return float("inf") if m else 0.0
    return (m - b) / b * 100.0

def report(group):
    b = baseline.get(group, {})
    m = measurement.get(group, {})
    print(f"  {group}:")
    for stat in ("p50", "p95", "p99"):
        if stat in b and stat in m:
            d = pct_delta(b[stat], m[stat])
            tag = "INFO"
            # Phase 2 will flip this tag to WARN/FAIL once thresholds are
            # calibrated. Phase 1 keeps the script informational only.
            print(f"    {stat}: {m[stat]:.3f} us (baseline {b[stat]:.3f} us, {d:+.1f}%) [{tag}]")

print("Collision mapper perf comparison (Chunk 1, Phase 1 informational):")
report("rebuild_us")
report("lookup_batch_us")
print("Phase 1: informational only; this script exits 0 regardless of regression.")
PY

exit 0
