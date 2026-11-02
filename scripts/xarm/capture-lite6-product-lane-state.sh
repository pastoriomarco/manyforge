#!/usr/bin/env bash
# I10/R gate for the bounded real-Lite6 product-state evidence client.
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob be39f0750a59f1b42e8225faa3f142f965ddde71.  Its bounded status,
# lifecycle-history, process identity, artifact-digest, and failure-result
# packaging maps to I5/H0 observability plus the I10/R product evidence gate.

set -euo pipefail

readonly reason_code="composable_cell_real_lite6_unavailable"
printf '%s\n' "${reason_code}: product-state capture returns at I10/R" >&2
exit 78
