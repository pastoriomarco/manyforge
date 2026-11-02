#!/usr/bin/env bash
# Materialize the exact RoboPlan 0.8 source closure and build ManyForge images.
#
# Usage:
#   ./scripts/setup-docker.sh
#   ./scripts/setup-docker.sh --prepare-only
#
# ROBOPLAN_REF may select another RoboPlan revision for development experiments.
# ProxSuite, jrl-cmakemodules, TOPPRA and nanoflann remain exact even in that
# mode, and the resulting image is explicitly non-qualifying.

set -euo pipefail
# The source-identity ledger records file modes as checked out with umask 0002
# (files 0664/0775). Use the same umask whatever the caller's, or a host with
# umask 0022 fails the identity preflight on a fresh checkout.
umask 0002

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_ROOT="$REPO_DIR/.docker"
ROBOPLAN_DIR="$SOURCE_ROOT/roboplan"
PROXSUITE_DIR="$SOURCE_ROOT/proxsuite"
JRL_CMAKEMODULES_DIR="$SOURCE_ROOT/jrl-cmakemodules"
TOPPRA_DIR="$SOURCE_ROOT/toppra"
NANOFLANN_DIR="$SOURCE_ROOT/nanoflann"

ROBOPLAN_URL="https://github.com/open-planning/roboplan.git"
ROBOPLAN_PINNED_REF="2a6b447e648b1a3337f10cbab995cda0c0a71d55"
ROBOPLAN_REF="${ROBOPLAN_REF:-$ROBOPLAN_PINNED_REF}"
PROXSUITE_URL="https://github.com/Simple-Robotics/proxsuite.git"
PROXSUITE_PINNED_REF="6ae71b84730bf30453d70764073d186f5fe7ad69"
JRL_CMAKEMODULES_URL="https://github.com/jrl-umi3218/jrl-cmakemodules.git"
JRL_CMAKEMODULES_PINNED_REF="984234828e97557b82d30262ffa3a851b6be2383"
TOPPRA_URL="https://github.com/hungpham2511/toppra.git"
TOPPRA_PINNED_REF="8121e9af72f5aa589b515aa15e0dc408344c3220"
NANOFLANN_URL="https://github.com/jlblancoc/nanoflann.git"
NANOFLANN_PINNED_REF="18f4edf753e813644a7a12711a86139777a8ef00"

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
    local resolved_sha
    resolved_sha=$(git -C "$target" rev-parse HEAD)
    printf '%s\n' "$ref" > "$ref_marker"
    printf '%s\n' "$resolved_sha" > "$sha_marker"
    info "$label source ready at $resolved_sha."
}

materialize_source RoboPlan "$ROBOPLAN_URL" "$ROBOPLAN_REF" "$ROBOPLAN_DIR"
materialize_source ProxSuite "$PROXSUITE_URL" "$PROXSUITE_PINNED_REF" "$PROXSUITE_DIR"
materialize_source jrl-cmakemodules "$JRL_CMAKEMODULES_URL" \
    "$JRL_CMAKEMODULES_PINNED_REF" "$JRL_CMAKEMODULES_DIR"
materialize_source TOPPRA "$TOPPRA_URL" "$TOPPRA_PINNED_REF" "$TOPPRA_DIR"
materialize_source nanoflann "$NANOFLANN_URL" "$NANOFLANN_PINNED_REF" "$NANOFLANN_DIR"

python3 "$ROBOPLAN_SOURCE_VERIFIER" \
    --ledger "$ROBOPLAN_SOURCE_LEDGER" \
    --source-id proxsuite_0_8_2_dependency \
    --root "$PROXSUITE_DIR" --content-kind source \
    || fail "ProxSuite source identity/license preflight failed."
python3 "$ROBOPLAN_SOURCE_VERIFIER" \
    --ledger "$ROBOPLAN_SOURCE_LEDGER" \
    --source-id jrl_cmakemodules_2_3_0_dependency \
    --root "$JRL_CMAKEMODULES_DIR" --content-kind source \
    || fail "jrl-cmakemodules source identity/license preflight failed."
python3 "$ROBOPLAN_SOURCE_VERIFIER" \
    --ledger "$ROBOPLAN_SOURCE_LEDGER" \
    --source-id toppra_0_6_10_ua1_dependency \
    --root "$TOPPRA_DIR" --content-kind source \
    || fail "TOPPRA source identity/license preflight failed."
python3 "$ROBOPLAN_SOURCE_VERIFIER" \
    --ledger "$ROBOPLAN_SOURCE_LEDGER" \
    --source-id nanoflann_2_0_1_dependency \
    --root "$NANOFLANN_DIR" --content-kind source \
    || fail "nanoflann source identity/license preflight failed."

ROBOPLAN_SOURCE_MODE=qualified
if [ "$ROBOPLAN_REF" = "$ROBOPLAN_PINNED_REF" ]; then
    python3 "$ROBOPLAN_SOURCE_VERIFIER" \
        --ledger "$ROBOPLAN_SOURCE_LEDGER" \
        --source-id roboplan_0_8_0_foundation \
        --root "$ROBOPLAN_DIR" --content-kind source \
        || fail "RoboPlan source identity/license preflight failed."
else
    ROBOPLAN_SOURCE_MODE=development_override
    warn "ROBOPLAN_REF override selected: this image cannot qualify a release."
fi

if [ "$prepare_only" = true ]; then
    if [ "$ROBOPLAN_SOURCE_MODE" = qualified ]; then
        info "Exact RoboPlan 0.8 source closure is ready."
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
