#!/usr/bin/env bash
# I10/R gate for the historical sole-owner Lite6 launcher.
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob e5bd0b119af0973fd79dad4d4c4f516d3b19a8d9.  Owner uniqueness,
# inactive startup, process supervision, controller/admission composition, and
# guarded shutdown return only after I4, I6-I8, and the I10 fake/offline gate.

set -euo pipefail

readonly reason_code="composable_cell_real_lite6_unavailable"
printf '%s\n' "${reason_code}: the sole-owner launcher returns at I10/R" >&2
exit 78
