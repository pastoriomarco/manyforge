#!/usr/bin/env bash
set -euo pipefail

WORKSPACE="${WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUTPUT_DIR="${MANYFORGE_INTERRUPT_QUALIFICATION_OUTPUT_DIR:-/tmp/manyforge-interrupt-qualification}"
MATRIX_PATH="${WORKSPACE}/verification/matrices/interrupt_qualification.yaml"

export PYTHONPATH="$WORKSPACE:$WORKSPACE/manyforge_behavior:${PYTHONPATH:-}"

cd "$WORKSPACE"
mapfile -t SCENARIOS < <(
  python3 - "$MATRIX_PATH" <<'PY'
import sys
from pathlib import Path
import yaml

matrix_path = Path(sys.argv[1])
data = yaml.safe_load(matrix_path.read_text(encoding="utf-8")) or {}
for scenario in data.get("scenarios", []):
    path = scenario if isinstance(scenario, str) else scenario.get("path")
    if path:
        print(path)
PY
)

mkdir -p "$OUTPUT_DIR"

for scenario in "${SCENARIOS[@]}"; do
  scenario_name="$(basename "$scenario" .scenario.yaml)"
  ./scripts/run-scenarios.py \
    "$scenario" \
    --launch local-process \
    --kernel-mode mock \
    --mock-execute-duration-s 0.5 \
    --output-dir "$OUTPUT_DIR/$scenario_name"
done
