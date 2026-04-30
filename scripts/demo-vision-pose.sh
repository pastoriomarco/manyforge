#!/usr/bin/env bash
# demo-vision-pose.sh — One-command vision-guided pose-pick demo
#
# Runs the Composer in the dev container, loads the vision-guided pose-pick
# example, and rewrites the deployment model endpoint to a host-reachable URL
# for the containerized runtime.
#
# Usage:
#   ./scripts/demo-vision-pose.sh
#   ./scripts/demo-vision-pose.sh --api-port 9010 --hmi-port 8082
#   ./scripts/demo-vision-pose.sh --model-base-url http://172.17.0.1:8000/v1
#   ./scripts/demo-vision-pose.sh --model-id qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge
#   ./scripts/demo-vision-pose.sh --rebuild
#
# Then open:
#   http://localhost:<api-port>        Composer UI
#   http://localhost:<hmi-port>        Viser 3D view

set -euo pipefail
cd "$(dirname "$0")/.."

CATALOG=/workspace/manyforge_behavior/resources/node_catalog.yaml
PROGRAM=/workspace/examples/vision_guided_pose_pick.program.yaml
DEPLOY=/workspace/examples/vision_guided_pose_pick.deployment.yaml
API_PORT=9000
HMI_PORT=8081
MODEL_BASE_URL=http://172.17.0.1:8000/v1
MODEL_ID=qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge
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
    --skip-bootstrap)
      BOOTSTRAP_MODE=skip
      shift
      ;;
    --rebuild)
      BOOTSTRAP_MODE=rebuild
      shift
      ;;
    -h|--help)
      sed -n '1,28p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

echo "Starting ManyForge vision-guided pose-pick demo..."
echo "Program:        $PROGRAM"
echo "Deployment:     $DEPLOY"
echo "API port:       $API_PORT"
echo "HMI port:       $HMI_PORT"
echo "Model base URL: $MODEL_BASE_URL"
echo "Model id:       $MODEL_ID"
echo "Bootstrap:      $BOOTSTRAP_MODE"
echo ""

exec docker compose run --rm \
  -e MANYFORGE_CATALOG_PATH="$CATALOG" \
  -e MANYFORGE_PROGRAM_PATH="$PROGRAM" \
  -e MANYFORGE_DEPLOY_PATH="$DEPLOY" \
  -e MANYFORGE_MODEL_BASE_URL="$MODEL_BASE_URL" \
  -e MANYFORGE_MODEL_ID="$MODEL_ID" \
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

  TMP_DEPLOY=$(mktemp -p /workspace .manyforge-demo-vision-pose.XXXXXX.deployment.yaml)
  trap '\''rm -f "$TMP_DEPLOY"'\'' EXIT

  python3 - <<PY
import yaml
from pathlib import Path

src = "${MANYFORGE_DEPLOY_PATH}"
dst = "${TMP_DEPLOY}"

with open(src, "r", encoding="utf-8") as f:
    data = yaml.safe_load(f)

registry_path = data.get("model_endpoint_profile_registry")
if registry_path and not Path(registry_path).is_absolute():
    data["model_endpoint_profile_registry"] = str(
        (Path(src).parent / registry_path).resolve()
    )

for endpoint in data.get("model_endpoints", {}).values():
    endpoint["base_url"] = "${MANYFORGE_MODEL_BASE_URL}"
    endpoint["model_id"] = "${MANYFORGE_MODEL_ID}"

with open(dst, "w", encoding="utf-8") as f:
    yaml.safe_dump(data, f, sort_keys=False)
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
    "path": "${MANYFORGE_PROGRAM_PATH}",
    "deploymentPath": "${TMP_DEPLOY}",
}))
PY

  curl -sf -X POST "http://localhost:${MANYFORGE_API_PORT}/api/program/load" \
    -H "Content-Type: application/json" \
    --data-binary @/tmp/manyforge-demo-load.json | python3 -m json.tool

  echo ""
  echo "=== ManyForge vision demo ready ==="
  echo "Composer UI:  http://localhost:'"${API_PORT}"'"
  echo "Viser 3D:     http://localhost:'"${HMI_PORT}"'"
  echo "Health:       http://localhost:'"${API_PORT}"'/api/health"
  echo "Program:      vision_guided_pose_pick"
  echo ""
  echo "Keep at least one HMI client connected before running."
  echo "Press Ctrl+C to stop."

  wait $PID
'
