#!/usr/bin/env bash
# Materialize the exact RoboPlan 0.7 source closure and build ManyForge images.
#
# Usage:
#   ./scripts/setup-docker.sh
#   ./scripts/setup-docker.sh --prepare-only
#
# ROBOPLAN_REF may select another RoboPlan revision for development experiments.
# ProxSuite and TOPPRA remain exact even in that mode, and the resulting image
# is explicitly non-qualifying.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_ROOT="$REPO_DIR/.docker"
ROBOPLAN_DIR="$SOURCE_ROOT/roboplan"
PROXSUITE_DIR="$SOURCE_ROOT/proxsuite"
TOPPRA_DIR="$SOURCE_ROOT/toppra"

ROBOPLAN_URL="https://github.com/open-planning/roboplan.git"
ROBOPLAN_PINNED_REF="099dfa5ccd80b191eb3d6da9f4795f808a63913f"
ROBOPLAN_REF="${ROBOPLAN_REF:-$ROBOPLAN_PINNED_REF}"
PROXSUITE_URL="https://github.com/Simple-Robotics/proxsuite.git"
PROXSUITE_PINNED_REF="b93d7778ffc3299d84b5cb0851022a29bf24a596"
TOPPRA_URL="https://github.com/hungpham2511/toppra.git"
TOPPRA_PINNED_REF="8121e9af72f5aa589b515aa15e0dc408344c3220"

ROBOPLAN_SOURCE_LEDGER="$REPO_DIR/verification/composable_cell_e1/external_delta_ledger.v1.json"
ROBOPLAN_SOURCE_VERIFIER="$REPO_DIR/scripts/ci/verify-external-source-closure.py"

info() { echo -e "\033[0;32m>>>\033[0m $1"; }
warn() { echo -e "\033[1;33m>>>\033[0m $1"; }
fail() { echo -e "\033[0;31m>>>\033[0m $1"; exit 1; }

prepare_only=false
if [ "$#" -gt 1 ]; then
    fail "Usage: $0 [--prepare-only]"
fi
case "${1:-}" in
    "") ;;
    --prepare-only) prepare_only=true ;;
    *) fail "Usage: $0 [--prepare-only]" ;;
esac

materialize_source() {
    local label="$1"
    local url="$2"
    local ref="$3"
    local target="$4"
    local initialized_submodule="${5:-}"
    local ref_marker="${target}.manyforge-ref"
    local sha_marker="${target}.manyforge-sha"
    local recorded_ref=""
    local reusable=false

    if [ -f "$ref_marker" ]; then
        recorded_ref=$(<"$ref_marker")
    fi
    if [ -d "$target/.git" ] && [ "$recorded_ref" = "$ref" ] \
        && [ -f "$sha_marker" ]; then
        local expected_sha actual_sha source_status
        expected_sha=$(<"$sha_marker")
        if actual_sha=$(git -C "$target" rev-parse HEAD 2>/dev/null) \
            && source_status=$(git -C "$target" status --porcelain=v1 \
                --untracked-files=all --ignore-submodules=none 2>/dev/null) \
            && [ "$actual_sha" = "$expected_sha" ] \
            && [ -z "$source_status" ]; then
            reusable=true
        fi
    fi

    if [ "$reusable" = true ]; then
        info "$label source is clean at $target ($(git -C "$target" rev-parse HEAD))."
        return
    fi

    if [ -e "$target" ]; then
        warn "$label source is absent, stale, or modified; recreating $target."
        rm -rf "$target"
    fi
    mkdir -p "$SOURCE_ROOT"
    git init -q "$target"
    git -C "$target" remote add origin "$url"
    git -C "$target" fetch --depth 1 origin "$ref"
    git -C "$target" checkout -q --detach FETCH_HEAD
    if [ -n "$initialized_submodule" ]; then
        git -C "$target" submodule update --init --depth 1 "$initialized_submodule"
    fi
    local resolved_sha
    resolved_sha=$(git -C "$target" rev-parse HEAD)
    printf '%s\n' "$ref" > "$ref_marker"
    printf '%s\n' "$resolved_sha" > "$sha_marker"
    info "$label source ready at $resolved_sha."
}

materialize_source RoboPlan "$ROBOPLAN_URL" "$ROBOPLAN_REF" "$ROBOPLAN_DIR"
materialize_source ProxSuite "$PROXSUITE_URL" "$PROXSUITE_PINNED_REF" \
    "$PROXSUITE_DIR" cmake-module
materialize_source TOPPRA "$TOPPRA_URL" "$TOPPRA_PINNED_REF" "$TOPPRA_DIR"

python3 "$ROBOPLAN_SOURCE_VERIFIER" \
    --ledger "$ROBOPLAN_SOURCE_LEDGER" \
    --source-id proxsuite_0_7_3_candidate_dependency \
    --root "$PROXSUITE_DIR" --content-kind source \
    || fail "ProxSuite source identity/license preflight failed."
python3 "$ROBOPLAN_SOURCE_VERIFIER" \
    --ledger "$ROBOPLAN_SOURCE_LEDGER" \
    --source-id proxsuite_cmake_module_candidate_dependency \
    --root "$PROXSUITE_DIR/cmake-module" --content-kind source \
    || fail "ProxSuite CMake-module identity/license preflight failed."
python3 "$ROBOPLAN_SOURCE_VERIFIER" \
    --ledger "$ROBOPLAN_SOURCE_LEDGER" \
    --source-id toppra_0_6_10_ua1_dependency \
    --root "$TOPPRA_DIR" --content-kind source \
    || fail "TOPPRA source identity/license preflight failed."

ROBOPLAN_SOURCE_MODE=qualified
if [ "$ROBOPLAN_REF" = "$ROBOPLAN_PINNED_REF" ]; then
    python3 "$ROBOPLAN_SOURCE_VERIFIER" \
        --ledger "$ROBOPLAN_SOURCE_LEDGER" \
        --source-id roboplan_0_7_0_ua1_foundation \
        --root "$ROBOPLAN_DIR" --content-kind source \
        || fail "RoboPlan source identity/license preflight failed."
else
    ROBOPLAN_SOURCE_MODE=development_override
    warn "ROBOPLAN_REF override selected: this image cannot qualify a release."
fi

if [ "$prepare_only" = true ]; then
    if [ "$ROBOPLAN_SOURCE_MODE" = qualified ]; then
        info "Exact RoboPlan 0.7 source closure is ready."
    else
        info "Exact dependencies and experimental RoboPlan source are ready."
    fi
    exit 0
fi

info "Building Docker images from the prepared source closure."
cd "$REPO_DIR"
ROBOPLAN_SOURCE_MODE="$ROBOPLAN_SOURCE_MODE" docker compose build

info "Setup complete."
echo ""
echo "  Start the dev environment:"
echo "    docker compose run --rm dev bash"
echo ""
echo "  Run retained tests in the container:"
echo "    ./scripts/test-all.sh retained"
