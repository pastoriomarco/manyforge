#!/usr/bin/env bash
# I10/R gate for the attended no-trajectory controller/gate evidence client.
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 51a999c6036b692a19abee64738431bdfa58a08c.  Its exact pose bound,
# controller/gate observations, provenance, and failure evidence return in the
# I10 fake/offline and attended R matrices after the I6-I8 binding cutover.

set -euo pipefail

readonly reason_code="composable_cell_real_lite6_unavailable"
printf '%s\n' "${reason_code}: controller/gate evidence returns at I10/R" >&2
exit 78
