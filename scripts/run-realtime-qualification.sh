#!/usr/bin/env bash
# Realtime architecture qualification evidence wrapper.
#
# This source-level probe records collision-resource policy, scheduler role
# expectations, and OS realtime acceptance visible to the current process. It
# does not replace native/Isaac loaded gates; those remain separate deployment
# evidence and are reported as not_run_by_probe by the payload.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "${REPO_ROOT}"
python3 manyforge_core/tools/collect_qualification_evidence.py \
  --repo-root "${REPO_ROOT}" \
  --lane "${MANYFORGE_QUALIFICATION_LANE:-rt6-local}" \
  --source "rt6-realtime-qualification" \
  --require-evidence \
  -- \
  python3 manyforge_core/tools/realtime_qualification_probe.py
