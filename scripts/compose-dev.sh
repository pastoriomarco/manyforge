#!/usr/bin/env bash
# compose-dev.sh — Start structural and Program-authoring Composer debug profiles
#
# These profiles serve the retained catalog, bounded filesystem, structural
# URDF, presentation, and strict Program Draft/Saved authoring surfaces. They
# do not Apply Programs, build or load qualified native execution products, or
# own runtime truth; those effectful routes fail closed.
#
# Historical runtime/E2E recipe:
# manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
# a84828d749fc2a63f409965cb8e1319773b1ef1c. H0/I5 supplies read-only runtime
# observation. N6-c3 restores authoring-only Program tree and artifact clients;
# authoritative Apply and full Program execution return at N7. Live execution
# state and Stop bind only to their new owners at N/I6 and P5/I8.
#
# Usage:
#   ./scripts/compose-dev.sh              # Start API (9000) + Vite HMR (3000)
#   ./scripts/compose-dev.sh --prod       # Start backend/static-UI debug mode
#   ./scripts/compose-dev.sh --stop       # Stop all composer services
#
# Dev mode:   http://localhost:3000  (Vite + hot reload, proxies Composer API)
# --prod:     http://localhost:9000  (Composer shell serves image-baked dist/)

set -euo pipefail
cd "$(dirname "$0")/.."

case "${1:-}" in
  --stop)
    docker compose --profile composer --profile dev-composer down
    exit 0
    ;;
  --prod)
    echo "Starting structural + Program-authoring Composer backend debug mode (port 9000)."
    echo "This structural debug profile does not launch Manager, Headless, Program-runner, or native product owners."
    docker compose --profile composer up --build -d
    echo ""
    echo "Composer running at http://localhost:9000"
    echo "Stop with: $0 --stop"
    ;;
  *)
    echo "Starting structural + Program-authoring Composer source/API debug mode (API :9000 + Vite :3000)."
    echo "This structural debug profile does not launch Manager, Headless, Program-runner, or native product owners."
    docker compose --profile dev-composer up --build -d
    echo ""
    echo "API:      http://localhost:9000"
    echo "Frontend: http://localhost:3000"
    echo "Stop with: $0 --stop"
    ;;
esac
