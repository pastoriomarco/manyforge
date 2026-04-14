#!/usr/bin/env bash
# demo-shape-pick.sh — One-command semantic shape-pick demo
#
# Runs the Composer in the dev container, loads the semantic shape-pick
# example, and rewrites the deployment model endpoint to a host-reachable URL
# for the containerized runtime. The requested shape can be switched at launch.
#
# Usage:
#   ./scripts/demo-shape-pick.sh
#   ./scripts/demo-shape-pick.sh --requested-shape cube
#   ./scripts/demo-shape-pick.sh --selection-request "Select the object on the left"
#   ./scripts/demo-shape-pick.sh --api-port 9010 --hmi-port 8082
#   ./scripts/demo-shape-pick.sh --model-base-url http://172.17.0.1:8000/v1
#   ./scripts/demo-shape-pick.sh --model-id gemma-4-E4B-it
#   ./scripts/demo-shape-pick.sh --rebuild
#
# Then open:
#   http://localhost:<api-port>        Composer UI
#   http://localhost:<hmi-port>        Viser 3D view

set -euo pipefail
cd "$(dirname "$0")/.."

CATALOG=/workspace/manyforge_behavior/resources/node_catalog.yaml
PROGRAM=/workspace/examples/semantic_shape_pick.program.yaml
DEPLOY=/workspace/examples/semantic_shape_pick.deployment.yaml
API_PORT=9000
HMI_PORT=8081
MODEL_BASE_URL=http://172.17.0.1:8000/v1
MODEL_ID=gemma-4-E4B-it
REQUESTED_SHAPE=sphere
SELECTION_REQUEST=""
BOOTSTRAP_MODE=auto

while [[ $# -gt 0 ]]; do
  case "$1" in
    --api-port)
      API_PORT="$2"
      shift 2
      ;;
    --hmi-port)
      HMI_PORT="$2"
      shift 2
      ;;
    --model-base-url)
      MODEL_BASE_URL="$2"
      shift 2
      ;;
    --model-id)
      MODEL_ID="$2"
      shift 2
      ;;
    --requested-shape)
      REQUESTED_SHAPE="$2"
      shift 2
      ;;
    --selection-request)
      SELECTION_REQUEST="$2"
      shift 2
      ;;
    --skip-bootstrap)
      BOOTSTRAP_MODE=skip
      shift
      ;;
    --rebuild)
      BOOTSTRAP_MODE=rebuild
      shift
      ;;
    -h|--help)
      sed -n '1,32p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ "$REQUESTED_SHAPE" != "sphere" && "$REQUESTED_SHAPE" != "cube" ]]; then
  echo "requested shape must be 'sphere' or 'cube'" >&2
  exit 2
fi

if [[ -z "$SELECTION_REQUEST" ]]; then
  SELECTION_REQUEST="Select the ${REQUESTED_SHAPE} object."
fi

echo "Starting ManyForge semantic shape-pick demo..."
echo "Program:         $PROGRAM"
echo "Deployment:      $DEPLOY"
echo "Requested shape: $REQUESTED_SHAPE"
echo "Selection req:   $SELECTION_REQUEST"
echo "API port:        $API_PORT"
echo "HMI port:        $HMI_PORT"
echo "Model base URL:  $MODEL_BASE_URL"
echo "Model id:        $MODEL_ID"
echo "Bootstrap:       $BOOTSTRAP_MODE"
echo ""

exec docker compose run --rm \
  -e MANYFORGE_CATALOG_PATH="$CATALOG" \
  -e MANYFORGE_PROGRAM_PATH="$PROGRAM" \
  -e MANYFORGE_DEPLOY_PATH="$DEPLOY" \
  -e MANYFORGE_MODEL_BASE_URL="$MODEL_BASE_URL" \
  -e MANYFORGE_MODEL_ID="$MODEL_ID" \
  -e MANYFORGE_REQUESTED_SHAPE="$REQUESTED_SHAPE" \
  -e MANYFORGE_SELECTION_REQUEST="$SELECTION_REQUEST" \
  -e MANYFORGE_BOOTSTRAP_MODE="$BOOTSTRAP_MODE" \
  -e MANYFORGE_API_PORT="$API_PORT" \
  -e MANYFORGE_HMI_PORT="$HMI_PORT" \
  -p "${API_PORT}:${API_PORT}" -p "${HMI_PORT}:${HMI_PORT}" dev bash -lc '
  set -euo pipefail

  rm -rf /opt/manyforge-frontend/dist 2>/dev/null || true

  case "${MANYFORGE_BOOTSTRAP_MODE}" in
    rebuild)
      echo "Building C++ backends..."
      ./scripts/test-all.sh cpp 2>&1 | tail -3 || echo "  (core build had warnings — continuing)"
      ./scripts/test-all.sh roboplan 2>&1 | tail -3 || echo "  (roboplan build failed — will use MockKernelClient)"
      echo ""
      ;;
    skip)
      echo "Skipping backend bootstrap."
      echo ""
      ;;
    auto)
      if [[ -d /tmp/manyforge-build/roboplan ]]; then
        echo "Using cached roboplan build from /tmp/manyforge-build/roboplan."
        echo ""
      else
        echo "Building C++ backends..."
        ./scripts/test-all.sh cpp 2>&1 | tail -3 || echo "  (core build had warnings — continuing)"
        ./scripts/test-all.sh roboplan 2>&1 | tail -3 || echo "  (roboplan build failed — will use MockKernelClient)"
        echo ""
      fi
      ;;
    *)
      echo "Unknown bootstrap mode: ${MANYFORGE_BOOTSTRAP_MODE}" >&2
      exit 2
      ;;
  esac

  export PYTHONPATH="/tmp/manyforge-build/roboplan:/workspace/manyforge_roboplan/python:${PYTHONPATH:-}"

  TMP_DEPLOY=$(mktemp -p /workspace .manyforge-demo-shape-pick.XXXXXX.deployment.yaml)
  TMP_PROGRAM=$(mktemp -p /workspace .manyforge-demo-shape-pick.XXXXXX.program.yaml)
  trap '\''rm -f "$TMP_DEPLOY" "$TMP_PROGRAM"'\'' EXIT

  python3 - <<PY
import yaml

deploy_src = "${MANYFORGE_DEPLOY_PATH}"
deploy_dst = "${TMP_DEPLOY}"
program_src = "${MANYFORGE_PROGRAM_PATH}"
program_dst = "${TMP_PROGRAM}"

with open(deploy_src, "r", encoding="utf-8") as f:
    deploy = yaml.safe_load(f)

for endpoint in deploy.get("model_endpoints", {}).values():
    endpoint["base_url"] = "${MANYFORGE_MODEL_BASE_URL}"
    endpoint["model_id"] = "${MANYFORGE_MODEL_ID}"

with open(deploy_dst, "w", encoding="utf-8") as f:
    yaml.safe_dump(deploy, f, sort_keys=False)

with open(program_src, "r", encoding="utf-8") as f:
    program = yaml.safe_load(f)

for param in program.get("parameters", []):
    if param.get("name") == "selection_request_text":
        param["default"] = "${MANYFORGE_SELECTION_REQUEST}"

with open(program_dst, "w", encoding="utf-8") as f:
    yaml.safe_dump(program, f, sort_keys=False)
PY

  python -m manyforge_composer \
    --catalog-path "$MANYFORGE_CATALOG_PATH" \
    --host 0.0.0.0 \
    --port "$MANYFORGE_API_PORT" \
    --hmi-port "$MANYFORGE_HMI_PORT" &
  PID=$!

  for i in $(seq 1 30); do
    curl -sf "http://localhost:${MANYFORGE_API_PORT}/api/health" >/dev/null 2>&1 && break
    sleep 0.5
  done

  python3 - <<PY >/tmp/manyforge-demo-load.json
import json
print(json.dumps({
    "path": "${TMP_PROGRAM}",
    "deploymentPath": "${TMP_DEPLOY}",
}))
PY

  curl -sf -X POST "http://localhost:${MANYFORGE_API_PORT}/api/program/load" \
    -H "Content-Type: application/json" \
    --data-binary @/tmp/manyforge-demo-load.json | python3 -m json.tool

  echo ""
  echo "=== ManyForge semantic shape-pick demo ready ==="
  echo "Composer UI:    http://localhost:'"${API_PORT}"'"
  echo "Viser 3D:       http://localhost:'"${HMI_PORT}"'"
  echo "Health:         http://localhost:'"${API_PORT}"'/api/health"
  echo "Selection req:  '"${SELECTION_REQUEST}"'"
  echo "Program:        semantic_shape_pick"
  echo ""
  echo "Keep at least one HMI client connected before running."
  echo "Press Ctrl+C to stop."

  wait $PID
'
