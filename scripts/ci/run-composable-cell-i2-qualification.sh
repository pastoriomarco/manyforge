#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

exec python3 \
  "${REPO_ROOT}/verification/composable_cell_i2/verify_qualification_manifest.py" \
  --manifest \
  "${REPO_ROOT}/verification/composable_cell_i2/qualification_manifest.v1.json" \
  "$@"
