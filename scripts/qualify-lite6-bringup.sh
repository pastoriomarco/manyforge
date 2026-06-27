#!/usr/bin/env bash
# Non-live Lite6 bring-up qualification.
#
# This gate proves the self-contained Lite6 model bundle, ManyMove runtime
# description equivalence, Robot Model Authority contracts, and ManyForge
# deployment/Isaac scaffold wiring. It deliberately does not replace live
# Composer UI, live Isaac, FoundationPose/trocar, or physical Lite6 validation.
#
# Host usage:
#   ./scripts/qualify-lite6-bringup.sh
#
# Container usage:
#   ./scripts/qualify-lite6-bringup.sh --in-container

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTAINER="${MANYFORGE_QUAL_CONTAINER:-manyforge-composer-ros-1}"
IN_CONTAINER=false
if [[ "${1:-}" == "--in-container" ]]; then
  IN_CONTAINER=true
fi

run_step() {
  echo
  echo "==> $*"
  "$@"
}

run_in_container() {
  local workspace="${WORKSPACE:-/workspace}"
  cd "${workspace}"

  run_step python3 -m py_compile \
    scripts/models/export_lite6_lite_gripper_model.py \
    scripts/models/capture_manymove_lite6_descriptions.py \
    scripts/models/compare_lite6_runtime_snapshot.py \
    scripts/models/validate_lite6_authority_runtime_contract.py \
    scripts/models/validate_lite6_static_manifest.py \
    scripts/ros/validate-isaac-lite6-smoke.py \
    scripts/ros/validate-isaac-lite6-smoke-artifact.py

  run_step bash -n \
    scripts/launch.sh \
    scripts/lib/scenarios.sh \
    scripts/ros/launch-isaac-topic-based-control.sh

  run_step ./scripts/models/compare_lite6_runtime_snapshot.py \
    --runtime-urdf manyforge_example_models/models/lite6_lite_gripper/manymove_runtime_capture/manymove_lite6_runtime.urdf \
    --runtime-srdf manyforge_example_models/models/lite6_lite_gripper/manymove_runtime_capture/manymove_lite6_runtime.srdf \
    --capture-json manyforge_example_models/models/lite6_lite_gripper/manymove_runtime_capture/manymove_lite6_runtime_capture.json \
    --json-report manyforge_example_models/models/lite6_lite_gripper/manymove_runtime_capture/compare_report.json

  run_step ./scripts/models/validate_lite6_static_manifest.py
  run_step ./scripts/models/validate_lite6_authority_runtime_contract.py --json

  local build_dir="${MANYFORGE_BUILD_DIR:-/tmp/manyforge-build}"
  export PYTHONPATH="${build_dir}/roboplan:${workspace}:${workspace}/manyforge_behavior:${workspace}/manyforge_roboplan/python:${PYTHONPATH:-}"
  run_step pytest -q \
    manyforge_behavior/tests/test_robot_model_authority.py \
    manyforge_behavior/tests/test_model_paths.py
  run_step pytest -q manyforge_behavior/tests/test_deployment.py -k "lite6"

  echo
  echo "Lite6 non-live model-authority qualification passed."
  echo "Live Composer UI, Isaac USD smoke, FoundationPose/trocar, and physical Lite6 validation remain deferred."
}

if [[ "${IN_CONTAINER}" == "true" ]]; then
  run_in_container
  exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "docker not found on PATH; rerun with --in-container inside the dev container." >&2
  exit 1
fi

if ! docker ps --format '{{.Names}}' | grep -qx "${CONTAINER}"; then
  echo "Container ${CONTAINER} is not running. Start the ManyForge Docker lane first." >&2
  exit 1
fi

cd "${REPO_ROOT}"
docker exec "${CONTAINER}" bash -lc "cd /workspace && ./scripts/qualify-lite6-bringup.sh --in-container"
