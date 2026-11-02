#!/usr/bin/env bash
# I10/R gate for the command-free live Lite6 evidence collector.
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 00b2e159807cc2cbb0b5dc0bf71be25b099fe22d.  Read-only/supervised
# capture, bounded raw probes, and provenance packaging remain acceptance
# inputs for the reconstructed I10 fake/offline and attended R evidence lanes.

set -euo pipefail

readonly reason_code="composable_cell_real_lite6_unavailable"
printf '%s\n' "${reason_code}: live evidence capture returns at I10/R" >&2
exit 78
