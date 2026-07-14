#!/usr/bin/env bash
# One-time setup for the ManyForge Docker dev environment.
#
# What it does:
#   1. Pre-clones RoboPlan (recursive submodule clone fails inside Docker)
#   2. Verifies the exact clean RoboPlan source revision
#   3. Builds the Docker image via docker compose
#
# Usage:
#   ./scripts/setup-docker.sh
#
#   # Optional: clone a different RoboPlan ref instead of the pinned SHA.
#   ROBOPLAN_REF=main ./scripts/setup-docker.sh
#   ROBOPLAN_REF=0.5.1 ./scripts/setup-docker.sh
#
# After setup:
#   docker compose run --rm dev bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROBOPLAN_DIR="$REPO_DIR/.docker/roboplan"
# Pinned to the reviewed commit referenced by RoboPlan's lightweight 0.5.1
# tag. Use the exact SHA for reproducible Docker builds while still allowing
# explicit ROBOPLAN_REF overrides for development experiments.
# Checked source patches are applied by the Dockerfiles after hash validation;
# .docker/roboplan remains an exact, disposable checkout of this commit.
# See docs/reference/ROBOPLAN_ADAPTER_NOTES.md for the source-level dependency.
ROBOPLAN_PINNED_REF="e06d1a2f1b749c38742aee9e401bbe97fa967f7b"
ROBOPLAN_REF="${ROBOPLAN_REF:-$ROBOPLAN_PINNED_REF}"
ROBOPLAN_REF_MARKER="$REPO_DIR/.docker/.manyforge-roboplan-ref"
ROBOPLAN_SHA_MARKER="$REPO_DIR/.docker/.manyforge-roboplan-sha"

info() { echo -e "\033[0;32m>>>\033[0m $1"; }
warn() { echo -e "\033[1;33m>>>\033[0m $1"; }
fail() { echo -e "\033[0;31m>>>\033[0m $1"; exit 1; }

# 1. Pre-clone RoboPlan
current_roboplan_ref=""
if [ -f "$ROBOPLAN_REF_MARKER" ]; then
    current_roboplan_ref=$(cat "$ROBOPLAN_REF_MARKER" || true)
fi

reuse_roboplan_checkout=false
if [ -d "$ROBOPLAN_DIR/.git" ] && [ "$current_roboplan_ref" = "$ROBOPLAN_REF" ] \
    && [ -f "$ROBOPLAN_SHA_MARKER" ]; then
    expected_roboplan_sha=$(cat "$ROBOPLAN_SHA_MARKER" || true)
    if actual_roboplan_sha=$(git -C "$ROBOPLAN_DIR" rev-parse HEAD 2>/dev/null) \
        && roboplan_status=$(git -C "$ROBOPLAN_DIR" status --porcelain=v1 \
            --untracked-files=all --ignore-submodules=none 2>/dev/null) \
        && roboplan_submodules=$(git -C "$ROBOPLAN_DIR" submodule status \
            --recursive 2>/dev/null) \
        && [ "$actual_roboplan_sha" = "$expected_roboplan_sha" ] \
        && [ -z "$roboplan_status" ] \
        && ! printf '%s\n' "$roboplan_submodules" | grep -Eq '^[+-U]'; then
        reuse_roboplan_checkout=true
    else
        warn "RoboPlan checkout is not the recorded clean source; recreating it."
    fi
fi

if [ "$reuse_roboplan_checkout" = true ]; then
    info "RoboPlan ref '$ROBOPLAN_REF' is present and source-clean at .docker/roboplan/."
    info "Resolved SHA: $(cat "$ROBOPLAN_SHA_MARKER")"
else
    info "Cloning RoboPlan ref '$ROBOPLAN_REF' (this takes a minute)..."
    rm -rf "$ROBOPLAN_DIR"
    mkdir -p "$REPO_DIR/.docker"
    git init "$ROBOPLAN_DIR"
    cd "$ROBOPLAN_DIR"
    git remote add origin https://github.com/open-planning/roboplan.git
    git fetch --depth 1 origin "$ROBOPLAN_REF"
    git checkout --detach FETCH_HEAD
    resolved_roboplan_sha=$(git rev-parse HEAD)
    git submodule update --init --recursive --depth 1
    cd "$REPO_DIR"

    printf '%s\n' "$ROBOPLAN_REF" > "$ROBOPLAN_REF_MARKER"
    printf '%s\n' "$resolved_roboplan_sha" > "$ROBOPLAN_SHA_MARKER"

    info "RoboPlan ready ($(du -sh "$ROBOPLAN_DIR" | cut -f1)); SHA $resolved_roboplan_sha."
fi

# 2. Build Docker image
info "Building Docker image (this takes several minutes on first run)..."
cd "$REPO_DIR"
docker compose build

info "Setup complete!"
echo ""
echo "  Start the dev environment:"
echo "    docker compose run --rm dev bash"
echo ""
echo "  Build and test C++ core:"
echo "    cd manyforge_core && mkdir -p build && cd build"
echo "    cmake .. && make -j\$(nproc) && ctest --output-on-failure"
echo ""
echo "  Run Python tests:"
echo "    pytest manyforge_behavior/tests/"
echo ""
echo "  Run all tests:"
echo "    ./scripts/test-all.sh"
