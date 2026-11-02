#!/usr/bin/env bash
# Hermes sandbox operator intent and typed cutover boundary.
#
# Exact implementation source:
# manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
# c4a68d59ea6de0820203bf257b8e72cfd0918101.
#
# Historical responsibility mapping:
#   lines 45-90    lane inputs, opt-in, and preflight;
#   green, yellow, red, step
#                  operator presentation for the future workflow;
#   lines 92-126   bounded staging-context construction;
#   lines 128-136  derived Dockerfile invocation;
#   lines 138-151  egress-policy merge and validation;
#   lines 153-212  OpenShell sandbox creation and timeout configuration;
#   lines 214-256  first-boot SDK/config/bridge/catalog/gateway evidence.
# These remain restoration requirements. The old Composer MCP catalog,
# bridge, gateway matrix, policy merge, image build, and sandbox lifecycle are
# absent rather than left below an early exit.

set -euo pipefail

_manyforge_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "${_manyforge_script_dir}/lib/composable-cell-runtime-availability.sh"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: Hermes setup returns with the qualified headless assistant and MCP clients."
manyforge_fail_composable_cell_runtime_unavailable
