#!/usr/bin/env bash
set -euo pipefail

WORKSPACE="${WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUTPUT_DIR="${MANYFORGE_SOAK_QUALIFICATION_OUTPUT_DIR:-/tmp/manyforge-soak-qualification}"
PORT_ARGS=()
if [[ -n "${MANYFORGE_QUALIFICATION_PORT:-}" ]]; then
  PORT_ARGS=(--port "${MANYFORGE_QUALIFICATION_PORT}")
fi

export PYTHONPATH="$WORKSPACE:$WORKSPACE/manyforge_behavior:${PYTHONPATH:-}"

cd "$WORKSPACE"
./scripts/run-scenarios.py \
  --matrix verification/matrices/soak_qualification.yaml \
  --launch local-process \
  --kernel-mode mock \
  --mock-execute-duration-s 0.2 \
  --output-dir "$OUTPUT_DIR" \
  "${PORT_ARGS[@]}" \
  "$@"
