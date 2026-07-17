#!/usr/bin/env bash
set -euo pipefail

WORKSPACE="${WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUTPUT_DIR="${MANYFORGE_NATIVE_QUALIFICATION_OUTPUT_DIR:-/tmp/manyforge-native-qualification}"
ROBOPLAN_BUILD_DIR="${MANYFORGE_ROBOPLAN_BUILD_DIR:-${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}/roboplan}"
PORT_ARGS=()
if [[ -n "${MANYFORGE_QUALIFICATION_PORT:-}" ]]; then
  PORT_ARGS=(--port "${MANYFORGE_QUALIFICATION_PORT}")
fi

export PYTHONPATH="$WORKSPACE:$WORKSPACE/manyforge_behavior:$WORKSPACE/manyforge_roboplan/python:${PYTHONPATH:-}"
if compgen -G "$ROBOPLAN_BUILD_DIR/manyforge_roboplan_py*.so" >/dev/null; then
  export PYTHONPATH="$ROBOPLAN_BUILD_DIR:$PYTHONPATH"
fi

cd "$WORKSPACE"
./scripts/run-scenarios.py \
  --matrix verification/matrices/native_live_evaluation.yaml \
  --launch local-process \
  --kernel-mode auto \
  --wait-timeout-s 30 \
  --output-dir "$OUTPUT_DIR" \
  "${PORT_ARGS[@]}" \
  "$@"
