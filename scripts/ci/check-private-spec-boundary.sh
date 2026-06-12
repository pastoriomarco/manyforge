#!/usr/bin/env bash
# check-private-spec-boundary.sh — boundary guard for the manyforge repo.
#
# manyforge is the deployment repository: it must remain self-sufficient
# at runtime without manyforge_specs (the private development repo)
# checked out on disk. This script enforces that boundary.
#
# It scans runtime/operator paths for references to `manyforge_specs`
# and fails (exit 1) if any are found outside an explicit allowlist.
# The allowlist exists for development-only entrypoints that legitimately
# require the specs repo (these MUST NOT be referenced from runtime
# scripts, docs, or code).
#
# Wire into CI:
#   .github/workflows/...   - run as a step; non-zero fails the build.
#   pre-commit              - or invoke from a pre-commit hook.
#   manual                  - bash scripts/ci/check-private-spec-boundary.sh
#
# To add a legitimate dev-only file to the allowlist, append to
# ALLOWLIST below with a one-line justification.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

# Files allowed to reference manyforge_specs. Each entry is a path
# (relative to repo root) and a one-line reason. Anything else that
# matches the search pattern is a boundary violation.
#
# Format: relative-path  # reason
ALLOWLIST=(
  "AGENTS.md                                                  # dual-purpose entrypoint: dev section legitimately names specs"
  "docs/operations/STACK_SETUP.md                             # boundary-describing doc: explicitly states specs is dev-only / not-required"
  "docs/operations/ORIN_STACK_SETUP.md                        # boundary-describing doc: marks the specs clone optional/maintainer-only"
  "scripts/dev-agent-session-start.sh                         # dev-only: seeds an OpenClaw agent sandbox with specs context"
  "scripts/regen-public-contracts.py                          # mentions specs in its own docstring (explaining why mirrors exist)"
  "scripts/ci/check-private-spec-boundary.sh                  # this file (mentions manyforge_specs in its own docstring/messages)"
)

# Build a grep-friendly allowlist filter (one path per line).
ALLOW_PATTERN="$(printf '%s\n' "${ALLOWLIST[@]}" | awk -F'#' '{gsub(/[[:space:]]+$/, "", $1); print $1}')"

# Search runtime/operator paths only. We deliberately do NOT scan everything
# under the repo root because some areas (`docs/adr/`, internal-only notes
# that may be added later, etc.) may legitimately reference specs.
# Add paths here as the repo's runtime surface grows.
RUNTIME_PATHS=(
  "README.md"
  "AGENTS.md"
  "docs/operations"
  "docs/guides"
  "docs/reference"
  "manyforge_assistant_bridge"
  "manyforge_composer/backend"
  "manyforge_behavior"
  "manyforge_core"
  "manyforge_roboplan"
  "manyforge_ros"
  "scripts"
  "examples"
)

VIOLATIONS=()

for path in "${RUNTIME_PATHS[@]}"; do
  [[ -e "${path}" ]] || continue
  # Recursive grep, exclude common non-source dirs and files.
  while IFS= read -r hit; do
    [[ -z "${hit}" ]] && continue
    file="${hit%%:*}"
    # Allowlist match?
    if echo "${ALLOW_PATTERN}" | grep -qxF "${file}"; then
      continue
    fi
    VIOLATIONS+=("${hit}")
  done < <(grep -rHnE "manyforge_specs" "${path}" \
            --include='*.py' --include='*.sh' --include='*.md' \
            --include='*.yaml' --include='*.yml' --include='*.json' \
            --include='*.ts' --include='*.tsx' --include='*.js' \
            --exclude-dir='.git' --exclude-dir='node_modules' \
            --exclude-dir='__pycache__' --exclude-dir='.venv' \
            --exclude-dir='.pytest_cache' --exclude-dir='build' \
            --exclude-dir='dist' 2>/dev/null || true)
done

if (( ${#VIOLATIONS[@]} > 0 )); then
  echo "ERROR: manyforge runtime path(s) reference 'manyforge_specs' (private dev repo)." >&2
  echo "       The deployment-side repo must remain self-sufficient." >&2
  echo "" >&2
  echo "Violations:" >&2
  printf '  %s\n' "${VIOLATIONS[@]}" >&2
  echo "" >&2
  echo "Fix by either:" >&2
  echo "  (a) Removing the reference if it's not load-bearing." >&2
  echo "  (b) Pointing at a public mirror in docs/reference/ instead." >&2
  echo "  (c) Moving the file into a clearly dev-only location and" >&2
  echo "      adding it to the ALLOWLIST in this script with a reason." >&2
  exit 1
fi

echo "OK: no private-spec boundary violations in manyforge runtime paths."
exit 0
