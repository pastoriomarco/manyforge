#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

cd "${REPO_ROOT}"

docker compose run --rm dev bash -lc '
  set -euo pipefail
  cd /workspace
  MANYFORGE_RUN_SOAK_TESTS=1 pytest -q manyforge_composer/tests/test_soak_qualification.py
'
