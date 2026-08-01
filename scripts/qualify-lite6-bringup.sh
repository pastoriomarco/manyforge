#!/usr/bin/env bash
# Gate for the mixed historical Lite6 bring-up qualification runner.
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 40db968d5e4c6462b40e656cf45fd1d9b0409872.  Its owner-neutral
# static artifact validator remains independently runnable; model retention,
# deployment, Isaac, real-Lite6, and tool-profile rows return at I3, I9/I,
# I10/R, and I11/T.

set -euo pipefail

readonly reason_code="composable_cell_lite6_bringup_qualification_unavailable"
printf '%s\n' "${reason_code}: complete Lite6 bring-up returns at I9/I and I10/R" >&2
exit 78
