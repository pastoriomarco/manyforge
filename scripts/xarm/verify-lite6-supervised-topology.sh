#!/usr/bin/env bash
# I10/R gate for the historical sole-owner topology verifier.
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 97944779782879bf49c9a93863a44a2170b0e1c0.  Exact owner count,
# endpoint type, and forbidden parallel-command-surface checks return against
# the reduced xarm_ros2 boundary in the I10 fake/offline and attended R gates.

set -euo pipefail

readonly reason_code="composable_cell_real_lite6_unavailable"
printf '%s\n' "${reason_code}: topology verification returns at I10/R" >&2
exit 78
