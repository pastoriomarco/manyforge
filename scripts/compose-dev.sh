#!/usr/bin/env bash
# compose-dev.sh — Start ManyForge Composer in dev mode
#
# Usage:
#   ./scripts/compose-dev.sh              # Start API (9000) + Vite HMR (3000)
#   ./scripts/compose-dev.sh --prod       # Start production mode (API serves built frontend on 9000)
#   ./scripts/compose-dev.sh --stop       # Stop all composer services
#
# Dev mode:   http://localhost:3000  (Vite + hot reload, proxies API to :9000)
# Prod mode:  http://localhost:9000  (FastAPI serves static dist/)
#
# Pick-and-place E2E test:
#   1. Start the composer (this script)
#   2. Open the UI in a browser
#   3. POST to load the example program:
#      curl -X POST http://localhost:9000/api/program/load \
#        -H 'Content-Type: application/json' \
#        -d '{"path":"/workspace/examples/pick_and_place_minimal.program.yaml",
#             "deploymentPath":"/workspace/manyforge_behavior/tests/fixtures/ur5_roboplan.deployment.yaml"}'
#   4. GET /api/program/tree to see the behavior tree
#   5. POST /api/cycle/run to start the cycle
#   6. Watch /ws/state WebSocket for live node statuses
#   7. POST /api/cycle/stop to stop
#   8. POST /api/cycle/reset to reset

set -euo pipefail
cd "$(dirname "$0")/.."

case "${1:-}" in
  --stop)
    docker compose --profile composer --profile dev-composer down
    exit 0
    ;;
  --prod)
    echo "Starting Composer in production mode (port 9000)..."
    docker compose --profile composer up --build -d
    echo ""
    echo "Composer running at http://localhost:9000"
    echo "Stop with: $0 --stop"
    ;;
  *)
    echo "Starting Composer in dev mode (API :9000 + Vite :3000)..."
    docker compose --profile dev-composer up --build -d
    echo ""
    echo "API:      http://localhost:9000"
    echo "Frontend: http://localhost:3000"
    echo "Stop with: $0 --stop"
    ;;
esac
