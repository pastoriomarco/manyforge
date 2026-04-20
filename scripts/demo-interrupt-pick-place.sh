#!/usr/bin/env bash
# demo-interrupt-pick-place.sh — One-command interrupt-focused pick-and-place demo
#
# Then open:
#   http://localhost:9000        Composer UI
#   http://localhost:8081        Viser 3D view
#
# Demo flow:
#   1. Start the stack with this script.
#   2. Run the cycle.
#   3. While a move is active, trigger an operator pause from another terminal:
#      curl -X POST http://localhost:9000/api/debug/signals/publish \
#        -H "Content-Type: application/json" \
#        -d '{"adapter":"demo_ui","signal":"pause_request","value":true,"staleAfterMs":1500,"mirrorKey":"demo/pause_request","metadata":{"reason":"operator_pause_demo"}}'
#   4. Inspect state:
#      curl http://localhost:9000/api/execution/digest | python3 -m json.tool
#   5. Resume after the handler completes and the cycle is in resumable hold:
#      curl -X POST http://localhost:9000/api/cycle/run

set -euo pipefail
cd "$(dirname "$0")/.."

CATALOG=/workspace/manyforge_behavior/resources/node_catalog.yaml
PROGRAM=/workspace/examples/interrupt_pick_and_place.program.yaml
DEPLOY=/workspace/examples/interrupt_pick_and_place.deployment.yaml

echo "Starting ManyForge Composer..."
echo "Program: $PROGRAM"
echo "Deployment: $DEPLOY"
echo ""

exec docker compose run --rm \
  -e MANYFORGE_CATALOG_PATH="$CATALOG" \
  -e MANYFORGE_PROGRAM_PATH="$PROGRAM" \
  -e MANYFORGE_DEPLOY_PATH="$DEPLOY" \
  -p 9000:9000 -p 8081:8081 dev bash -lc '
  set -euo pipefail

  rm -rf /opt/manyforge-frontend/dist 2>/dev/null || true

  echo "Building C++ backends..."
  ./scripts/test-all.sh cpp 2>&1 | tail -3 || echo "  (core build had warnings — continuing)"
  ./scripts/test-all.sh roboplan 2>&1 | tail -3 || echo "  (roboplan build failed — will use MockKernelClient)"
  echo ""

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
  echo "=== ManyForge interrupt demo ready ==="
  echo "Composer UI:  http://localhost:9000"
  echo "Viser 3D:     http://localhost:8081"
  echo "Health:       http://localhost:9000/api/health"
  echo ""
  echo "Trigger operator pause from another terminal with:"
  echo "  curl -X POST http://localhost:9000/api/debug/signals/publish \\"
  echo "    -H \"Content-Type: application/json\" \\"
  echo "    -d '\''{\"adapter\":\"demo_ui\",\"signal\":\"pause_request\",\"value\":true,\"staleAfterMs\":1500,\"mirrorKey\":\"demo/pause_request\",\"metadata\":{\"reason\":\"operator_pause_demo\"}}'\''"
  echo ""
  echo "Inspect digest with:"
  echo "  curl http://localhost:9000/api/execution/digest | python3 -m json.tool"
  echo ""
  echo "Resume after hold with:"
  echo "  curl -X POST http://localhost:9000/api/cycle/run"
  echo ""
  echo "Press Ctrl+C to stop."

  wait $PID
'
