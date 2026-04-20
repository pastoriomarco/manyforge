#!/usr/bin/env bash
# Run the core ManyForge qualification matrix in a production-like local process.
set -euo pipefail
cd "$(dirname "$0")/.."

OUTPUT_DIR="${MANYFORGE_QUALIFICATION_OUTPUT_DIR:-/tmp/manyforge-qualification}"
CATALOG_PATH="${MANYFORGE_CATALOG_PATH:-manyforge_behavior/resources/node_catalog.yaml}"

python3 scripts/run-scenarios.py \
  --matrix verification/matrices/core_qualification.yaml \
  --launch local-process \
  --catalog-path "$CATALOG_PATH" \
  --kernel-mode mock \
  --mock-execute-duration-s 0.2 \
  --workspace-root "$(pwd)" \
  --output-dir "$OUTPUT_DIR" \
  "$@"
