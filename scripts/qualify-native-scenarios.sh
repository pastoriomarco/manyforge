#!/usr/bin/env bash
set -euo pipefail

WORKSPACE="${WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUTPUT_DIR="${MANYFORGE_NATIVE_QUALIFICATION_OUTPUT_DIR:-/tmp/manyforge-native-qualification}"

export PYTHONPATH="$WORKSPACE:$WORKSPACE/manyforge_behavior:$WORKSPACE/manyforge_roboplan/python:${PYTHONPATH:-}"

cd "$WORKSPACE"
./scripts/run-scenarios.py \
  --matrix verification/matrices/native_live_evaluation.yaml \
  --launch local-process \
  --kernel-mode auto \
  --wait-timeout-s 30 \
  --output-dir "$OUTPUT_DIR"
