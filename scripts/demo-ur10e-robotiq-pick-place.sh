#!/usr/bin/env bash
# demo-ur10e-robotiq-pick-place.sh — One-command UR10e + Robotiq preview demo
#
# Usage:
#   ./scripts/demo-ur10e-robotiq-pick-place.sh
#
# Then open:
#   http://localhost:9000        Composer UI
#   http://localhost:8081        Viser 3D view
#
# This preview uses a self-contained example model bundle under
# `manyforge_example_models`, so it no longer depends on a host ROS workspace.

set -euo pipefail
cd "$(dirname "$0")/.."

CATALOG=/workspace/manyforge_behavior/resources/node_catalog.yaml
PROGRAM=/workspace/examples/pick_and_place_ur10e_robotiq.program.yaml
DEPLOY=/workspace/examples/pick_and_place_ur10e_robotiq.deployment.yaml

echo "Starting ManyForge UR10e + Robotiq preview..."
echo "Program:    $PROGRAM"
echo "Deployment: $DEPLOY"
echo ""

exec docker compose run --rm \
  -e MANYFORGE_CATALOG_PATH="$CATALOG" \
  -e MANYFORGE_PROGRAM_PATH="$PROGRAM" \
  -e MANYFORGE_DEPLOY_PATH="$DEPLOY" \
  -p 9000:9000 -p 8081:8081 dev bash -lc '
  set -euo pipefail

  rm -rf /opt/manyforge-frontend/dist 2>/dev/null || true

  echo "Building native RoboPlan backend..."
  BUILD_DIR=/tmp/manyforge-build/roboplan
  mkdir -p "$BUILD_DIR"

  PINOCCHIO_CMAKE_DIR=$(find /usr -name "pinocchioConfig.cmake" -printf "%h\n" 2>/dev/null | head -1 || true)
  if [[ -n "$PINOCCHIO_CMAKE_DIR" ]]; then
    PINOCCHIO_PREFIX=$(dirname "$(dirname "$(dirname "$PINOCCHIO_CMAKE_DIR")")")
  else
    PINOCCHIO_PREFIX=""
  fi

  BOOST_ROOT=""
  if [[ -n "$PINOCCHIO_PREFIX" && -f "${PINOCCHIO_PREFIX}/include/boost/version.hpp" ]]; then
    BOOST_ROOT="$PINOCCHIO_PREFIX"
  fi

  PYBIND11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())" 2>/dev/null || true)

  (
    cd "$BUILD_DIR"
    CMAKE_PREFIX_PATH="/usr/local${PINOCCHIO_PREFIX:+:${PINOCCHIO_PREFIX}}:${CMAKE_PREFIX_PATH:-}" \
      BOOST_ROOT="$BOOST_ROOT" \
      BOOST_INCLUDEDIR="${BOOST_ROOT:+${BOOST_ROOT}/include}" \
      BOOST_LIBRARYDIR="${BOOST_ROOT:+${BOOST_ROOT}/lib}" \
      cmake /workspace/manyforge_roboplan -DCMAKE_BUILD_TYPE=Release \
      ${PYBIND11_DIR:+-Dpybind11_DIR="$PYBIND11_DIR"} 2>&1 | tail -10
    make -j"$(nproc)" 2>&1 | tail -10
  ) || echo "  (roboplan build failed — will use MockKernelClient)"
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
  echo "=== ManyForge UR10e + Robotiq preview ready ==="
  echo "Composer UI:  http://localhost:9000"
  echo "Viser 3D:     http://localhost:8081"
  echo "Health:       http://localhost:9000/api/health"
  echo ""
  echo "Press Ctrl+C to stop."

  wait $PID
'
