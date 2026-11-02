#!/usr/bin/env bash
# Historical Composer runtime/viewer/historian soak-qualification entrypoint.
#
# Exact full-Composer source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 4fb80cbeaf5b05ff79d3ac825291202b32ecc432.  That command selected six
# effectful soak cases with MANYFORGE_RUN_SOAK_TESTS=1.  The three retained
# structural cache tests remain ordinary Composer tests; running only those
# under this historical command would be a false-green soak claim.
#
# The runtime/viewer/historian cases return with their C++ owners and thin
# clients at I5/H0, I6/N, and I9/I.  Keep this gate effect-free until then.

set -euo pipefail

builtin printf '%s\n' \
  'composable_cell_runtime_unavailable: historical Composer runtime/viewer/historian soak qualification returns at I5/H0, I6/N, and I9/I; retained structural cache tests remain in the ordinary Composer suite' \
  >&2
exit 78
