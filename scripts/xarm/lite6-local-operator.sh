#!/usr/bin/env bash
# I10/R gate for the attended local Lite6 thin client.
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob f40bd225a63aeec34c6ccb23bc0eed2ac5fc0457.  Explicit opt-in,
# per-action confirmation, principal/context attribution, status observation,
# and evidence retention map to I7/I8 plus the I10/R attended client rows.

set -euo pipefail

readonly reason_code="composable_cell_real_lite6_unavailable"
printf '%s\n' "${reason_code}: the attended local client returns at I10/R" >&2
exit 78
