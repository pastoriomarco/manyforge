#!/usr/bin/env bash
# check-public-contracts-fresh.sh — guard that auto-generated reference
# docs in `docs/reference/` are up-to-date with the live code.
#
# The three docs (REST_API.md, MCP_RUNTIME_CONTRACT.md,
# ASSISTANT_PROVIDER_CONTRACT.md) are produced by
# `scripts/regen-public-contracts.py` from FastAPI's openapi() output and
# the FastMCP tool registry. They drift silently when factory functions
# get renamed or new tools/routes are added without rerunning the
# regenerator — exactly the failure that landed `build_app` →
# `create_app` and `build_mcp_server` → `create_mcp_server` rename in the
# code, with the regen script + the docs left frozen against the old
# names for an unknown number of months.
#
# This script runs the regenerator in `--check` mode; exit 1 on drift.
#
# It MUST run inside the manyforge dev container — the regen script
# imports `manyforge_composer.backend.app` and
# `manyforge_composer.backend.mcp_server`, which require the dev
# image's installed dependencies (FastAPI, FastMCP, the kernel client,
# etc.). Running on the host without those deps would itself produce
# a failure stub and a misleading drift result.
#
# Wire into CI:
#   .github/workflows/...   - run as a step; non-zero fails the build.
#   pre-commit              - or invoke from a pre-commit hook.
#   manual                  - bash scripts/ci/check-public-contracts-fresh.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

if [[ -n "${INSIDE_MANYFORGE_DEV_CONTAINER:-}" ]]; then
  # Already inside the dev image — run the check directly.
  exec python3 scripts/regen-public-contracts.py --check
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
  python3 scripts/regen-public-contracts.py --check
