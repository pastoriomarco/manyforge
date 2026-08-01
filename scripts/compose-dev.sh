#!/usr/bin/env bash
# compose-dev.sh — Start structural-only Composer frontend/API debug profiles
#
# These profiles serve the retained catalog, bounded filesystem, structural
# URDF, and presentation surfaces. They do not build or load qualified native
# execution products, and every former runtime route fails closed.
#
# Historical runtime/E2E recipe:
# manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
# a84828d749fc2a63f409965cb8e1319773b1ef1c. Its Program load/tree and cycle
# control interactions return as thin clients at H0/I5 and N/I6; live runtime
# state and Stop bind only to their new headless owners at N/I6 and P5/I8.
#
# Usage:
#   ./scripts/compose-dev.sh              # Start API (9000) + Vite HMR (3000)
#   ./scripts/compose-dev.sh --prod       # Start backend/static-UI debug mode
#   ./scripts/compose-dev.sh --stop       # Stop all composer services
#
# Dev mode:   http://localhost:3000  (Vite + hot reload, proxies structural API)
# --prod:     http://localhost:9000  (structural shell serves image-baked dist/)

set -euo pipefail
cd "$(dirname "$0")/.."

case "${1:-}" in
  --stop)
    docker compose --profile composer --profile dev-composer down
    exit 0
    ;;
  --prod)
    echo "Starting structural-only Composer backend debug mode (port 9000)."
    echo "Runtime and product lanes remain intentionally unavailable."
    docker compose --profile composer up --build -d
    echo ""
    echo "Composer running at http://localhost:9000"
    echo "Stop with: $0 --stop"
    ;;
  *)
    echo "Starting structural-only Composer source/API debug mode (API :9000 + Vite :3000)."
    echo "Runtime and product lanes remain intentionally unavailable."
    docker compose --profile dev-composer up --build -d
    echo ""
    echo "API:      http://localhost:9000"
    echo "Frontend: http://localhost:3000"
    echo "Stop with: $0 --stop"
    ;;
esac
