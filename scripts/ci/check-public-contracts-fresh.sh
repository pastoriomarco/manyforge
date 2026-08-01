#!/usr/bin/env bash
# check-public-contracts-fresh.sh — guard that the generated structural
# Composer REST reference is up-to-date with the retained FastAPI app.
#
# `docs/reference/REST_API.md` is produced by
# `scripts/regen-public-contracts.py` from
# `create_app(ComposerState()).openapi()`. MCP, assistant, viewer, scene,
# execution, lifecycle, and runtime contracts are intentionally not generated
# while those product owners are unavailable during the composable-cell
# cutover.
#
# The historical no-argument form represented the removed aggregate contract
# check and is unavailable.  The retained REST-only check requires the
# explicit `structural-rest` selector and exits 1 on drift.
#
# It MUST run inside the manyforge dev container — the regen script
# imports the retained Composer app and structural state, which require the
# dev image's installed FastAPI and ManyForge Python dependencies. The
# generator fails closed on import or construction errors.
#
# Wire into CI:
#   .github/workflows/...   - run as a step; non-zero fails the build.
#   pre-commit              - or invoke from a pre-commit hook.
#   manual                  - bash scripts/ci/check-public-contracts-fresh.sh structural-rest

set -euo pipefail

readonly UNAVAILABLE_REASON_CODE="composable_cell_runtime_unavailable"
readonly LEGACY_AGGREGATE_UNAVAILABLE_MESSAGE="${UNAVAILABLE_REASON_CODE}: aggregate public-contract freshness is unavailable while its former runtime contract owners are removed; use the explicit structural-rest selector for the retained Composer REST projection."

if (( $# == 0 )); then
  builtin printf '%s\n' "${LEGACY_AGGREGATE_UNAVAILABLE_MESSAGE}" >&2
  exit 78
fi

if (( $# != 1 )) || [[ "$1" != "structural-rest" ]]; then
  builtin printf 'Usage: %s structural-rest\n' "$0" >&2
  exit 64
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

if [[ -n "${INSIDE_MANYFORGE_DEV_CONTAINER:-}" ]]; then
  # Already inside the dev image — run the check directly.
  exec python3 scripts/regen-public-contracts.py structural-rest --check
fi

# Host invocation: shell out to docker compose. The `dev` service mounts
# the repo at /workspace and has all the runtime deps installed.
if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: docker not found on PATH; cannot run check-public-contracts-fresh.sh" >&2
  echo "       (regen script needs the manyforge dev container's Python env)" >&2
  exit 2
fi

exec docker compose run --rm \
  -e INSIDE_MANYFORGE_DEV_CONTAINER=1 \
  dev \
  python3 scripts/regen-public-contracts.py structural-rest --check
