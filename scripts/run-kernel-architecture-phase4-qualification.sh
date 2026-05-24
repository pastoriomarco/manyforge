#!/usr/bin/env bash
# Kernel Architecture Sprint Phase 4 release-qualification harness.
#
# Default behavior is release-strict: missing TSan / soak / interrupt / visual /
# live Isaac evidence fails the final verifier. Pass --allow-incomplete for an
# offline development run that still produces an explicit "not release ready"
# report when evidence is missing.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${MANYFORGE_PHASE4_OUT_DIR:-${REPO_ROOT}/build/phase4-qualification}"
ALLOW_INCOMPLETE=false
IN_CONTAINER=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --allow-incomplete)
      ALLOW_INCOMPLETE=true
      shift
      ;;
    --in-container)
      IN_CONTAINER=true
      shift
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

mkdir -p "${OUT_DIR}"

status_to_arg() {
  local status="$1"
  case "${status}" in
    passed|failed|not_run)
      printf '%s' "${status}"
      ;;
    *)
      echo "invalid evidence status: ${status}" >&2
      exit 2
      ;;
  esac
}

json_success_status() {
  local path="$1"
  python3 - "$path" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
try:
    payload = json.loads(path.read_text(encoding="utf-8"))
except Exception:
    print("failed")
    raise SystemExit(0)
print("passed" if bool(payload.get("success")) else "failed")
PY
}

run_collision_matrix() {
  local out_json="${OUT_DIR}/collision-engine-matrix.json"
  if [[ "${IN_CONTAINER}" == "true" ]]; then
    MANYFORGE_DIRECT_ENGINE_QUAL_JSON_OUT="${out_json}" \
      "${REPO_ROOT}/scripts/run-collision-engine-qualification.sh" --in-container \
      >/dev/null
  else
    MANYFORGE_DIRECT_ENGINE_QUAL_JSON_OUT="${out_json}" \
      "${REPO_ROOT}/scripts/run-collision-engine-qualification.sh" \
      >/dev/null
  fi
  printf '%s' "${out_json}"
}

run_optional_step() {
  local enabled="$1"
  local name="$2"
  shift 2
  if [[ "${enabled}" != "1" ]]; then
    printf 'not_run'
    return 0
  fi
  if "$@" >"${OUT_DIR}/${name}.log" 2>&1; then
    printf 'passed'
  else
    printf 'failed'
  fi
}

collision_json="$(run_collision_matrix)"

if [[ "${MANYFORGE_PHASE4_RUN_TSAN:-0}" == "1" ]]; then
  if [[ "${IN_CONTAINER}" == "true" ]]; then
    race_status="$(run_optional_step "1" "tsan-collision-engine" \
      "${REPO_ROOT}/scripts/ci/run-collision-engine-tsan.sh")"
  else
    race_status="$(run_optional_step "1" "tsan-collision-engine" \
      docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm dev \
      bash -lc "./scripts/ci/run-collision-engine-tsan.sh")"
  fi
else
  race_status="not_run"
fi

if [[ "${MANYFORGE_PHASE4_RUN_SOAK:-0}" == "1" ]]; then
  if [[ "${IN_CONTAINER}" == "true" ]]; then
    if MANYFORGE_RUN_SOAK_TESTS=1 pytest -q \
      "${REPO_ROOT}/manyforge_composer/tests/test_soak_qualification.py" \
      >"${OUT_DIR}/soak.log" 2>&1; then
      soak_status="passed"
    else
      soak_status="failed"
    fi
  else
    soak_status="$(run_optional_step "1" "soak" \
      "${REPO_ROOT}/scripts/ci/run-soak-qualification.sh")"
  fi
else
  soak_status="not_run"
fi

if [[ "${MANYFORGE_PHASE4_RUN_INTERRUPT:-0}" == "1" ]]; then
  interrupt_cmd='
    set -euo pipefail
    build_dir="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/core-phase4-interrupt"
    mkdir -p "${build_dir}"
    cd "${build_dir}"
    cmake /workspace/manyforge_core -DBUILD_TESTING=ON >/tmp/phase4_interrupt_cmake.log
    cmake --build . --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" >/tmp/phase4_interrupt_build.log
    ctest --output-on-failure -R "ExecutionKernel|InterruptManager|StateMachine"
  '
  if [[ "${IN_CONTAINER}" == "true" ]]; then
    if bash -lc "${interrupt_cmd}" >"${OUT_DIR}/interrupt.log" 2>&1; then
      interrupt_status="passed"
    else
      interrupt_status="failed"
    fi
  elif docker compose -f "${REPO_ROOT}/docker-compose.yaml" run --rm dev \
       bash -lc "${interrupt_cmd}" >"${OUT_DIR}/interrupt.log" 2>&1; then
    interrupt_status="passed"
  else
    interrupt_status="failed"
  fi
else
  interrupt_status="not_run"
fi

if [[ "${MANYFORGE_PHASE4_RUN_VISUAL:-0}" == "1" ]]; then
  visual_summary="${OUT_DIR}/visual-summary.json"
  if [[ -z "${MANYFORGE_PHASE4_COMPOSER_URL:-}" ]]; then
    visual_status="failed"
    echo "MANYFORGE_PHASE4_COMPOSER_URL is required for visual evidence" \
      >"${OUT_DIR}/visual.log"
  elif "${REPO_ROOT}/scripts/qualify-visual-state-live.py" \
        --base-url "${MANYFORGE_PHASE4_COMPOSER_URL}" \
        --summary-json "${visual_summary}" \
        >"${OUT_DIR}/visual.log" 2>&1; then
    if [[ -n "${MANYFORGE_PHASE4_VISUAL_LOAD_EVIDENCE_JSON:-}" ]]; then
      cp "${MANYFORGE_PHASE4_VISUAL_LOAD_EVIDENCE_JSON}" \
        "${OUT_DIR}/visual-load-evidence.json"
      visual_load_status="$(json_success_status "${OUT_DIR}/visual-load-evidence.json")"
    else
      visual_load_status="${MANYFORGE_PHASE4_VISUAL_LOAD_EVIDENCE:-not_run}"
      if [[ "${visual_load_status}" == "passed" ]]; then
        cat >"${OUT_DIR}/visual-load-evidence.json" <<'JSON'
{"success": true, "source": "operator_attested_validation_load"}
JSON
      fi
    fi
    if [[ "${visual_load_status}" == "passed" ]]; then
      visual_status="passed"
    else
      visual_status="failed"
      {
        echo "visual route probe passed but validation-load evidence is ${visual_load_status}"
        echo "set MANYFORGE_PHASE4_VISUAL_LOAD_EVIDENCE_JSON to a JSON artifact with success=true"
      } >"${OUT_DIR}/visual-load.log"
    fi
  else
    visual_status="failed"
  fi
else
  visual_status="not_run"
fi

if [[ -n "${MANYFORGE_PHASE4_LIVE_ISAAC_EVIDENCE_JSON:-}" ]]; then
  cp "${MANYFORGE_PHASE4_LIVE_ISAAC_EVIDENCE_JSON}" \
    "${OUT_DIR}/live-isaac-summary.json"
  live_isaac_status="$(json_success_status "${OUT_DIR}/live-isaac-summary.json")"
else
  live_isaac_status="$(status_to_arg "${MANYFORGE_PHASE4_LIVE_ISAAC_EVIDENCE:-not_run}")"
fi

verify_args=(
  --repo-root "${REPO_ROOT}"
  --collision-json "${collision_json}"
  --out "${OUT_DIR}/phase4-release-report.json"
  --race-evidence "${race_status}"
  --soak-evidence "${soak_status}"
  --interrupt-evidence "${interrupt_status}"
  --visual-evidence "${visual_status}"
  --live-isaac-evidence "${live_isaac_status}"
)

if [[ -f "${OUT_DIR}/visual-summary.json" ]]; then
  verify_args+=(--visual-summary-json "${OUT_DIR}/visual-summary.json")
fi
if [[ -f "${OUT_DIR}/visual-load-evidence.json" ]]; then
  verify_args+=(--visual-load-evidence-json "${OUT_DIR}/visual-load-evidence.json")
fi
if [[ -f "${OUT_DIR}/live-isaac-summary.json" ]]; then
  verify_args+=(--live-isaac-summary-json "${OUT_DIR}/live-isaac-summary.json")
fi

if [[ "${ALLOW_INCOMPLETE}" == "true" ]]; then
  verify_args+=(--allow-incomplete)
fi

"${REPO_ROOT}/scripts/verify-kernel-architecture-phase4.py" "${verify_args[@]}"
