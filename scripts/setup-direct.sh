#!/usr/bin/env bash
# Direct-assistant typed cutover boundary.
#
# Exact implementation source:
# manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
# 7876ff39c3b79d85da19f5703a928861d9bf8f19.
#
# Historical responsibility mapping:
#   lines 41-53   operator argument parsing -> future headless client;
#   green, yellow, red
#                 operator presentation for the future workflow;
#   lines 63-81   vLLM and proxy probes -> future model/provider client;
#   lines 83-90   old Composer program-route probe -> headless client;
#   lines 92-107  bridge environment provisioning -> deployment workflow;
#   lines 109-126 shared projection import probe -> qualified client package;
#   lines 128-150 bridge health and summary -> assistant readiness client.
# No historical option or environment contract is parsed, and no venv,
# package, process, import, or network operation occurs here now.

set -euo pipefail

_manyforge_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "${_manyforge_script_dir}/lib/composable-cell-runtime-availability.sh"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: Direct assistant setup returns with the qualified headless assistant client."
manyforge_fail_composable_cell_runtime_unavailable
