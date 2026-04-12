#!/usr/bin/env bash
# demo.sh — One-command pick-and-place demo
#
# Usage:
#   ./scripts/demo.sh                     # Full pick-and-place (12-step)
#   ./scripts/demo.sh --simple            # Minimal 3-step test fixture
#
# Then open:
#   http://localhost:9000        Composer UI (behavior tree editor)
#   http://localhost:8081        Viser 3D view (robot visualization)
#
# API cheat sheet:
#   curl -X POST localhost:9000/api/cycle/run     # start cycle
#   curl -X POST localhost:9000/api/cycle/stop    # stop
#   curl -X POST localhost:9000/api/cycle/reset   # reset
#   curl localhost:9000/api/program/tree           # get behavior tree

set -euo pipefail
cd "$(dirname "$0")/.."

CATALOG=/workspace/manyforge_behavior/resources/node_catalog.yaml
DEPLOY=/workspace/examples/pick_and_place.deployment.yaml

if [[ "${1:-}" == "--simple" ]]; then
  PROGRAM=/workspace/manyforge_behavior/tests/fixtures/pick_and_place.program.yaml
else
  PROGRAM=/workspace/examples/pick_and_place.program.yaml
fi

echo "Starting ManyForge Composer..."
echo "Program: $PROGRAM"
echo ""

exec docker compose run --rm \
  -e MANYFORGE_CATALOG_PATH="$CATALOG" \
  -e MANYFORGE_PROGRAM_PATH="$PROGRAM" \
  -e MANYFORGE_DEPLOY_PATH="$DEPLOY" \
  -p 9000:9000 -p 8081:8081 dev bash -lc '
  set -euo pipefail

  # Remove stale frontend from Docker image — use volume-mounted dev build
  rm -rf /opt/manyforge-frontend/dist 2>/dev/null || true

  # Build C++ backends (manyforge_core → manyforge_roboplan_py)
  # These are best-effort: if roboplan fails, the composer falls back to MockKernelClient
  echo "Building C++ backends..."
  ./scripts/test-all.sh cpp 2>&1 | tail -3 || echo "  (core build had warnings — continuing)"
  ./scripts/test-all.sh roboplan 2>&1 | tail -3 || echo "  (roboplan build failed — will use MockKernelClient)"
  echo ""

  # Ensure roboplan Python paths are available
  export PYTHONPATH="/tmp/manyforge-build/roboplan:/workspace/manyforge_roboplan/python:${PYTHONPATH:-}"

  python -m manyforge_composer --catalog-path "$MANYFORGE_CATALOG_PATH" --host 0.0.0.0 --port 9000 --hmi-port 8081 &
  PID=$!

  for i in $(seq 1 30); do
    curl -sf http://localhost:9000/api/health > /dev/null 2>&1 && break
    sleep 0.5
  done

  python3 - <<PY >/tmp/manyforge-demo-load.json
import json
print(json.dumps({
    "path": "${MANYFORGE_PROGRAM_PATH}",
    "deploymentPath": "${MANYFORGE_DEPLOY_PATH}",
}))
PY

  curl -sf -X POST http://localhost:9000/api/program/load \
    -H "Content-Type: application/json" \
    --data-binary @/tmp/manyforge-demo-load.json | python3 -m json.tool

  echo ""
  echo "=== ManyForge Composer ready ==="
  echo "Composer UI:  http://localhost:9000"
  echo "Viser 3D:     http://localhost:8081"
  echo "Health:       http://localhost:9000/api/health"
  echo ""
  echo "Press Ctrl+C to stop."

  wait $PID
'
