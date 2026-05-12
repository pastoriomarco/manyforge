#!/usr/bin/env bash
# One-time setup for the ManyForge Docker dev environment.
#
# What it does:
#   1. Pre-clones RoboPlan (recursive submodule clone fails inside Docker)
#   2. Strips .git directories (reduces image size)
#   3. Builds the Docker image via docker compose
#
# Usage:
#   ./scripts/setup-docker.sh
#
#   # Optional: clone a RoboPlan branch, tag, or commit instead of the
#   # default pinned release.
#   ROBOPLAN_REF=main ./scripts/setup-docker.sh
#   ROBOPLAN_REF=cylinder-and-mesh ./scripts/setup-docker.sh
#   ROBOPLAN_REF=69dd5f9f91870af8937b029ca1f4890e65ce98c5 ./scripts/setup-docker.sh
#
# After setup:
#   docker compose run --rm dev bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROBOPLAN_DIR="$REPO_DIR/.docker/roboplan"
ROBOPLAN_VERSION="0.3.0"
ROBOPLAN_REF="${ROBOPLAN_REF:-$ROBOPLAN_VERSION}"
ROBOPLAN_REF_MARKER="$ROBOPLAN_DIR/.manyforge-roboplan-ref"
ROBOPLAN_SHA_MARKER="$ROBOPLAN_DIR/.manyforge-roboplan-sha"

info() { echo -e "\033[0;32m>>>\033[0m $1"; }
warn() { echo -e "\033[1;33m>>>\033[0m $1"; }
fail() { echo -e "\033[0;31m>>>\033[0m $1"; exit 1; }

# 1. Pre-clone RoboPlan
current_roboplan_version=""
if [ -f "$ROBOPLAN_DIR/roboplan/package.xml" ]; then
    current_roboplan_version=$(sed -n 's:.*<version>\(.*\)</version>.*:\1:p' "$ROBOPLAN_DIR/roboplan/package.xml" | head -n 1 || true)
fi
current_roboplan_ref=""
if [ -f "$ROBOPLAN_REF_MARKER" ]; then
    current_roboplan_ref=$(cat "$ROBOPLAN_REF_MARKER" || true)
fi

if [ -d "$ROBOPLAN_DIR/scripts" ] && [ "$current_roboplan_ref" = "$ROBOPLAN_REF" ]; then
    info "RoboPlan ref '$ROBOPLAN_REF' already present at .docker/roboplan/ — skipping clone."
    if [ -f "$ROBOPLAN_SHA_MARKER" ]; then
        info "Resolved SHA: $(cat "$ROBOPLAN_SHA_MARKER")"
    fi
    info "To force a refresh, delete .docker/roboplan/ and re-run."
elif [ -d "$ROBOPLAN_DIR/scripts" ] \
    && [ -z "$current_roboplan_ref" ] \
    && [ "$ROBOPLAN_REF" = "$ROBOPLAN_VERSION" ] \
    && [ "$current_roboplan_version" = "$ROBOPLAN_VERSION" ]; then
    info "RoboPlan $ROBOPLAN_VERSION already present at .docker/roboplan/ — skipping clone."
    printf '%s\n' "$ROBOPLAN_REF" > "$ROBOPLAN_REF_MARKER"
    info "To force a refresh, delete .docker/roboplan/ and re-run."
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

    info "Stripping .git directories..."
    find "$ROBOPLAN_DIR" -name ".git" -type d -exec rm -rf {} + 2>/dev/null || true
    find "$ROBOPLAN_DIR" -name ".git" -type f -delete 2>/dev/null || true
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
