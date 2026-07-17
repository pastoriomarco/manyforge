#!/usr/bin/env bash
# Run the core ManyForge qualification matrix in a production-like local process.
set -euo pipefail

WORKSPACE="${WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
export PYTHONPATH="$WORKSPACE:$WORKSPACE/manyforge_behavior:${PYTHONPATH:-}"
cd "$WORKSPACE"

OUTPUT_DIR="${MANYFORGE_QUALIFICATION_OUTPUT_DIR:-/tmp/manyforge-qualification}"
CATALOG_PATH="${MANYFORGE_CATALOG_PATH:-manyforge_behavior/resources/node_catalog.yaml}"
PORT_ARGS=()
if [[ -n "${MANYFORGE_QUALIFICATION_PORT:-}" ]]; then
  PORT_ARGS=(--port "${MANYFORGE_QUALIFICATION_PORT}")
fi

python3 scripts/run-scenarios.py \
  --matrix verification/matrices/core_qualification.yaml \
  --launch local-process \
  --catalog-path "$CATALOG_PATH" \
  --kernel-mode mock \
  --mock-execute-duration-s 0.2 \
  --workspace-root "$(pwd)" \
  --output-dir "$OUTPUT_DIR" \
  "${PORT_ARGS[@]}" \
  "$@"
