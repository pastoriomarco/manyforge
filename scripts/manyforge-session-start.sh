#!/usr/bin/env bash
# ManyForge session startup script.
# Run this after a reboot or when starting a new manyforge dev session.
# It restores the sandbox to the state needed for agent tasks.
#
# Usage: ./scripts/manyforge-session-start.sh [sandbox-name]
#
# Prerequisites:
#   - vLLM is serving the selected local model on port 8000
#   - Run the relevant NemoClaw-Thor local-provider/profile setup first
#   - Then run this script

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SPECS_DIR="${MANYFORGE_SPECS_ROOT:-$REPO_DIR/../manyforge_specs}"
SANDBOX_NAME="${1:-thor-assistant}"
NEMOCLAW_THOR="$HOME/workspaces/nemoclaw/src/NemoClaw-Thor"

echo "=== ManyForge Session Startup ==="
echo "Sandbox: $SANDBOX_NAME"
echo "Repo:    $REPO_DIR"
echo "Specs:   $SPECS_DIR"
echo ""

if [[ ! -d "$SPECS_DIR" ]]; then
    echo "ERROR: manyforge_specs not found at $SPECS_DIR"
    echo "Set MANYFORGE_SPECS_ROOT to the specs checkout before seeding an agent sandbox."
    exit 1
fi

# --- 1. Verify Thor is healthy ---
echo ">>> [1/4] Checking Thor system status..."
MODEL_PROFILE="${THOR_MODEL_PROFILE:-qwen3.5-27b-claude-distilled-nvfp4}"
if ! "$NEMOCLAW_THOR/status.sh" "$MODEL_PROFILE" 2>&1 | grep -q "All 19 checks passed"; then
    echo "    System not healthy. Run:"
    echo "    cd $NEMOCLAW_THOR && ./configure-local-provider.sh $MODEL_PROFILE"
    exit 1
fi
echo "    OK — all 19 checks passed."

# --- 2. Upload shared resources ---
echo ">>> [2/4] Uploading shared resources to sandbox..."

openshell sandbox upload "$SANDBOX_NAME" \
    "$SPECS_DIR/" /sandbox/specs/ 2>&1 | grep -E "✓|Error"

openshell sandbox upload "$SANDBOX_NAME" \
    "$REPO_DIR/manyforge_core/include/manyforge_core/" \
    /sandbox/contracts/manyforge_core/include/manyforge_core/ 2>&1 | grep -E "✓|Error"

# Upload engineering reference if available
REFERENCE="$SPECS_DIR/docs/reference/REFERENCE.md"
if [[ -f "$REFERENCE" ]]; then
    openshell sandbox upload "$SANDBOX_NAME" "$REFERENCE" /sandbox/reference/REFERENCE.md 2>&1 | grep -E "✓|Error"
else
    echo "    WARNING: $REFERENCE not found — skipping reference upload"
fi

echo "    Shared resources uploaded."

# --- 3. Restore worker workspaces for any known active tasks ---
echo ">>> [3/4] Restoring worker workspaces..."

ACTIVE_TASKS=()
# Add task IDs here when starting new Phase 1 batches, e.g.:
# Add task IDs here when starting new sessions, e.g.:
# ACTIVE_TASKS=("s3-bindings")
ACTIVE_TASKS=()

for TASK_ID in "${ACTIVE_TASKS[@]}"; do
    if [[ -d "/tmp/manyforge-baseline-${TASK_ID}" ]]; then
        echo "    Baseline exists for ${TASK_ID}, re-uploading from baseline..."
        openshell sandbox upload "$SANDBOX_NAME" \
            "/tmp/manyforge-baseline-${TASK_ID}/manyforge/" \
            "/sandbox/work/${TASK_ID}/manyforge/" 2>&1 | grep -E "✓|Error"
    else
        echo "    No baseline for ${TASK_ID}, running setup-worker.sh..."
        "$SCRIPT_DIR/setup-worker.sh" "$TASK_ID" "$SANDBOX_NAME"
    fi
done

echo "    Worker workspaces restored."

# --- 4. Summary ---
echo ""
echo ">>> [4/4] Session ready."
echo ""
echo "  Sandbox shared resources:"
echo "    /sandbox/specs/           — manyforge ADRs and specs"
echo "    /sandbox/contracts/       — frozen contract headers"
echo "    /sandbox/reference/       — ManyForge engineering reference"
echo ""
echo "  Active task workspaces:"
for TASK_ID in "${ACTIVE_TASKS[@]}"; do
    echo "    /sandbox/work/${TASK_ID}/"
done
echo ""
echo "  To dispatch an agent task:"
echo "    ssh ... 'HOME=/sandbox openclaw agent --agent main --local --json"
echo "      --session-id <task-id> --timeout 1200 -m \"<prompt>\"'"
echo ""
echo "=== Done ==="
