#!/usr/bin/env bash
# Typed boundary for the historical OpenClaw/NemoClaw end-to-end smoke.
#
# Exact source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob ec71c58fd9af671835cf9fd24921261a4b80b439.
#
# Restoration assignments from that source:
#   lines 23-38              mode/config validation;
#   cleanup/record_smoke_result
#                            reversible provider state and durable evidence;
#   extract_json             bounded OpenClaw result decoding;
#   provider_base_url/assert_sandbox_base_url/run_agent_json
#                            sandbox/provider client checks;
#   direct_smoke             model-only assistant smoke;
#   verify_productized_plugin
#                            plugin identity/provenance verification;
#   mux_smoke                historical MCP/mux tool workflow; and
#   run_requested_smoke      external-loop restoration plus N7 orchestration.
# The pure decoding and provenance intent returns in focused offline tooling;
# the old provider, mux, plugin, Composer, and sandbox wires are not dormant.

set -euo pipefail

_manyforge_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "${_manyforge_script_dir}/lib/composable-cell-runtime-availability.sh"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: the OpenClaw smoke requires a strict-v1 external adapter and N7 live-product qualification."
manyforge_fail_composable_cell_runtime_unavailable
