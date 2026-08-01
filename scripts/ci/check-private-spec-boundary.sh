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

export LC_ALL=C

fail_internal() {
  printf 'ERROR: private-spec boundary check could not complete: %s\n' "$1" >&2
  exit 2
}

SCRIPT_SOURCE="${BASH_SOURCE[0]}"
if [[ "${SCRIPT_SOURCE}" == */* ]]; then
  SCRIPT_DIR="${SCRIPT_SOURCE%/*}"
else
  SCRIPT_DIR="."
fi
if ! REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd -P)"; then
  fail_internal "cannot resolve the repository root"
fi
if ! cd -- "${REPO_ROOT}"; then
  fail_internal "cannot enter the repository root"
fi

MAX_REPO_PATH_BYTES=4096
MAX_VIOLATION_FILES=256
# `ulimit -f` uses 1024-byte blocks in bash. This caps one grep inventory at
# 2 MiB before any path is read into shell state.
MAX_GREP_OUTPUT_BLOCKS=2048

# Files allowed to reference manyforge_specs. Each entry is one exact path
# relative to the repository root. Keep the inline one-line justification;
# anything else that matches the search pattern is a boundary violation.
ALLOWLIST=(
  "AGENTS.md" # dual-purpose entrypoint: dev section legitimately names specs
  "docs/operations/STACK_SETUP.md" # boundary-describing doc: explicitly states specs is dev-only / not-required
  "docs/operations/ORIN_STACK_SETUP.md" # boundary-describing doc: marks the specs clone optional/maintainer-only
  "scripts/dev-agent-session-start.sh" # dev-only: seeds an OpenClaw agent sandbox with specs context
  "scripts/regen-public-contracts.py" # mentions specs in its own docstring (explaining why mirrors exist)
  "scripts/ci/check-private-spec-boundary.sh" # this file (mentions manyforge_specs in its own docstring/messages)
  "scripts/qualification/runtime_collision_schedule_baseline.md" # dev-only: exact offline qualification runbook binds immutable Specs identities
  "scripts/qualification/step18_offline_proof.md" # dev-only: exact Step 18 proof campaign binds immutable Specs identities
  "scripts/run-step18-loaded-kernel-qualification.sh" # dev-only: binds the loaded-kernel gate to the Specs contract identity
)

validate_relative_path() {
  local value="$1"
  local label="$2"
  if [[ -z "${value}" || "${value}" == /* || "${value}" == "." || \
        "${value}" == ".." || "/${value}/" == *"/../"* || \
        "/${value}/" == *"/./"* || "${value}" == *$'\n'* || \
        "${value}" == *$'\r'* ]]; then
    fail_internal "${label} is not a safe repository-relative path"
  fi
  if (( ${#value} > MAX_REPO_PATH_BYTES )); then
    fail_internal "${label} exceeds the ${MAX_REPO_PATH_BYTES}-byte path bound"
  fi
}

is_allowlisted() {
  local candidate="$1"
  local allowed
  for allowed in "${ALLOWLIST[@]}"; do
    if [[ "${candidate}" == "${allowed}" ]]; then
      return 0
    fi
  done
  return 1
}

VALIDATED_ALLOWLIST=()
for allowed in "${ALLOWLIST[@]}"; do
  validate_relative_path "${allowed}" "allowlist entry"
  if [[ ! -f "${allowed}" || -L "${allowed}" ]]; then
    fail_internal "allowlist target is missing, non-regular, or a symlink: ${allowed}"
  fi
  for existing in "${VALIDATED_ALLOWLIST[@]}"; do
    if [[ "${allowed}" == "${existing}" ]]; then
      fail_internal "duplicate allowlist entry: ${allowed}"
    fi
  done
  VALIDATED_ALLOWLIST+=("${allowed}")
done

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
VIOLATIONS_TRUNCATED=0
VALIDATED_RUNTIME_PATHS=()

for required_tool in grep mktemp rm; do
  if ! command -v "${required_tool}" >/dev/null 2>&1; then
    fail_internal "required tool is unavailable: ${required_tool}"
  fi
done

if ! HITS_FILE="$(mktemp /tmp/manyforge-private-spec-boundary-hits.XXXXXX)"; then
  fail_internal "cannot create the bounded grep inventory"
fi
ERROR_FILE=""
cleanup() {
  local original_status=$?
  local cleanup_failed=0
  trap - EXIT
  if ! rm -f -- "${HITS_FILE}"; then
    cleanup_failed=1
  fi
  if [[ -n "${ERROR_FILE}" ]] && ! rm -f -- "${ERROR_FILE}"; then
    cleanup_failed=1
  fi
  if (( cleanup_failed )); then
    printf '%s\n' \
      "ERROR: private-spec boundary check could not remove its temporary files" >&2
    exit 2
  fi
  exit "${original_status}"
}
trap cleanup EXIT
if ! ERROR_FILE="$(mktemp /tmp/manyforge-private-spec-boundary-errors.XXXXXX)"; then
  fail_internal "cannot create the bounded grep diagnostic file"
fi

for path in "${RUNTIME_PATHS[@]}"; do
  validate_relative_path "${path}" "runtime scan root"
  if [[ ! -e "${path}" ]]; then
    continue
  fi
  if [[ -L "${path}" ]]; then
    fail_internal "runtime scan root is a symlink: ${path}"
  fi
  if [[ ! -f "${path}" && ! -d "${path}" ]]; then
    fail_internal "runtime scan root is not a regular file or directory: ${path}"
  fi
  for existing in "${VALIDATED_RUNTIME_PATHS[@]}"; do
    if [[ "${path}" == "${existing}" ]]; then
      fail_internal "duplicate runtime scan root: ${path}"
    fi
  done
  VALIDATED_RUNTIME_PATHS+=("${path}")

  if ! : >"${HITS_FILE}"; then
    fail_internal "cannot reset the bounded grep inventory"
  fi
  if ! : >"${ERROR_FILE}"; then
    fail_internal "cannot reset the bounded grep diagnostic file"
  fi
  search_status=0
  if (
    if ! ulimit -f "${MAX_GREP_OUTPUT_BLOCKS}"; then
      exit 125
    fi
    exec grep -rFlZ \
      --include='*.py' --include='*.sh' --include='*.md' \
      --include='*.yaml' --include='*.yml' --include='*.json' \
      --include='*.ts' --include='*.tsx' --include='*.js' \
      --include='*.c' --include='*.cc' --include='*.cpp' \
      --include='*.h' --include='*.hh' --include='*.hpp' \
      --include='CMakeLists.txt' --include='*.cmake' \
      --exclude-dir='.git' --exclude-dir='node_modules' \
      --exclude-dir='__pycache__' --exclude-dir='.venv' \
      --exclude-dir='.pytest_cache' --exclude-dir='build' \
      --exclude-dir='dist' -- "manyforge_specs" "${path}"
  ) >"${HITS_FILE}" 2>"${ERROR_FILE}"; then
    search_status=0
  else
    search_status=$?
  fi
  if (( search_status != 0 && search_status != 1 )); then
    fail_internal "grep failed while scanning ${path} (status ${search_status})"
  fi

  while :; do
    file=""
    if IFS= read -r -d '' file; then
      if [[ -z "${file}" ]]; then
        fail_internal "grep returned an empty path while scanning ${path}"
      fi
      validate_relative_path "${file}" "grep result"
      if [[ -f "${path}" ]]; then
        if [[ "${file}" != "${path}" ]]; then
          fail_internal "grep returned a path outside the file scan root: ${path}"
        fi
      elif [[ "${file}" != "${path}/"* ]]; then
        fail_internal "grep returned a path outside the directory scan root: ${path}"
      fi
      if [[ ! -f "${file}" || -L "${file}" ]]; then
        fail_internal "grep result is missing, non-regular, or a symlink: ${file}"
      fi
      if is_allowlisted "${file}"; then
        continue
      fi
      if (( ${#VIOLATIONS[@]} >= MAX_VIOLATION_FILES )); then
        VIOLATIONS_TRUNCATED=1
        break
      fi
      VIOLATIONS+=("${file}")
    else
      if [[ -n "${file}" ]]; then
        fail_internal "grep returned a non-NUL-terminated path inventory"
      fi
      break
    fi
  done <"${HITS_FILE}"

  if (( VIOLATIONS_TRUNCATED )); then
    break
  fi
done

if (( ${#VIOLATIONS[@]} > 0 )); then
  echo "ERROR: manyforge runtime path(s) reference 'manyforge_specs' (private dev repo)." >&2
  echo "       The deployment-side repo must remain self-sufficient." >&2
  echo "" >&2
  echo "Violating files:" >&2
  printf '  %s\n' "${VIOLATIONS[@]}" >&2
  if (( VIOLATIONS_TRUNCATED )); then
    printf '  ... more than %d violating files; output truncated ...\n' \
      "${MAX_VIOLATION_FILES}" >&2
  fi
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
