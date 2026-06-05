#!/usr/bin/env bash
# setup-hermes.sh — Phase 4 deliverable per THREE-LANE-MIGRATION-PLAN.md.
#
# Provision the Hermes lane from a clean state:
#   1. Tear down any prior sandbox.
#   2. Onboard a fresh sandbox with the Hermes agent (NemoClaw blueprint).
#   3. Apply shared + hermes-overlay policy.
#   4. Inject mcp_servers.manyforge config into /sandbox/.hermes/config.yaml.
#   5. Provision API_SERVER_KEY (currently from env; Open question Q6 of the
#      plan tracks where the canonical secret store lives).
#   6. Probe the two Hermes session APIs (sessions_chat vs runs) and the
#      vLLM --tool-call-parser hermes wire shape (Phase 4 probes).
#   7. Start the Hermes bridge on :8300 (when HERMES_LANE_PHASE4_ENABLED).
#
# Phase 4 status: this script ships as scaffolding. The probes (#6) and
# bridge start (#7) are documented but the actual implementation lives
# behind the HERMES_LANE_PHASE4_ENABLED feature flag and the empirical
# Phase 4 work has not landed yet.
set -euo pipefail

NAME="${1:-hermes-assistant}"

green() { echo -e "\033[32m✓\033[0m $1"; }
yellow() { echo -e "\033[33m!\033[0m $1"; }
red() { echo -e "\033[31m✗\033[0m $1" >&2; }

if [[ "${HERMES_LANE_PHASE4_ENABLED:-false}" != "true" ]]; then
  yellow "HERMES_LANE_PHASE4_ENABLED is not true."
  yellow "This script is scaffolding only. The Hermes lane bridge and"
  yellow "probes are documented in the plan but not yet implemented."
  yellow ""
  yellow "What this script will do (when enabled):"
  yellow "  1. Tear down sandbox '${NAME}'"
  yellow "  2. Onboard fresh with --agent hermes (restricted policy)"
  yellow "  3. Apply manyforge-egress-shared + manyforge-hermes.overlay"
  yellow "  4. Inject mcp_servers.manyforge into /sandbox/.hermes/config.yaml"
  yellow "     (from manyforge/lanes/hermes/mcp_servers_config.yaml)"
  yellow "  5. Verify Hermes lists mcp_manyforge_* tools"
  yellow "  6. Run Phase 4 probes:"
  yellow "     - vLLM --tool-call-parser hermes shape (5-case probe)"
  yellow "     - sessions_chat vs /v1/runs head-to-head (3-case probe)"
  yellow "  7. Start manyforge_hermes_bridge on :8300"
  exit 0
fi

# === Phase 4 implementation goes here ===
# 1. Tear down
red "Phase 4 not yet implemented. Set HERMES_LANE_PHASE4_ENABLED=true once"
red "the Phase 4 work has landed."
exit 1
