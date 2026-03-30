#!/usr/bin/env bash
# Upload a filtered copy of the manyforge repo to a sandbox worker workspace.
#
# Usage: ./setup-worker.sh <task-id> [sandbox-name]
#
# Creates:
#   /sandbox/work/<task-id>/manyforge/  (in sandbox)
#   /tmp/manyforge-baseline-<task-id>/  (local snapshot for later diff)

set -euo pipefail

TASK_ID="${1:?Usage: setup-worker.sh <task-id> [sandbox-name]}"
SANDBOX_NAME="${2:-thor-assistant}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BASELINE_DIR="/tmp/manyforge-baseline-${TASK_ID}"
STAGING_DIR="$(mktemp -d "/tmp/manyforge-stage-${TASK_ID}-XXXXXX")"

echo ">>> Staging filtered repo copy..."
rsync -a \
  --exclude='build/' \
  --exclude='.git/' \
  --exclude='scripts/' \
  --exclude='*.o' \
  --exclude='*.a' \
  "$REPO_DIR/" "$STAGING_DIR/manyforge/"

echo ">>> Saving local baseline to ${BASELINE_DIR}..."
rm -rf "$BASELINE_DIR"
cp -r "$STAGING_DIR" "$BASELINE_DIR"

echo ">>> Uploading to sandbox /sandbox/work/${TASK_ID}/..."
openshell sandbox upload "$SANDBOX_NAME" "$STAGING_DIR/manyforge/" "/sandbox/work/${TASK_ID}/manyforge/"

rm -rf "$STAGING_DIR"

echo ">>> Worker workspace ready at /sandbox/work/${TASK_ID}/manyforge/"
echo ">>> Baseline snapshot at ${BASELINE_DIR}/manyforge/"
