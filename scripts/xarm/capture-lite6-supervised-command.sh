#!/usr/bin/env bash
# I10/R gate for the attended lifecycle-command evidence client.
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 6c90e0e8049ef6ec59065c3638c4b89c5eeee777.  Exact confirmation,
# timeout/transient bounds, caller context, postcondition, and provenance rows
# return through I7/I8 and the I10/R attended behavior ledger.

set -euo pipefail

readonly reason_code="composable_cell_real_lite6_unavailable"
printf '%s\n' "${reason_code}: attended command capture returns at I10/R" >&2
exit 78
