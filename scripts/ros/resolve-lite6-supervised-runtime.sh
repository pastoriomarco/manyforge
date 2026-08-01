#!/usr/bin/env bash
# Typed pre-effect boundary for the unqualified supervised Lite6 runtime.

set -euo pipefail

readonly REASON_CODE="composable_cell_runtime_unavailable"
readonly RESTORATION_GATE="I10/R"

echo "${REASON_CODE}: supervised Lite6 runtime resolution requires the ${RESTORATION_GATE} restoration gate" >&2
exit 78
