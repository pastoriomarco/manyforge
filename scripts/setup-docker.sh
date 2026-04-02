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
# After setup:
#   docker compose run --rm dev bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROBOPLAN_DIR="$REPO_DIR/.docker/roboplan"

info() { echo -e "\033[0;32m>>>\033[0m $1"; }
warn() { echo -e "\033[1;33m>>>\033[0m $1"; }
fail() { echo -e "\033[0;31m>>>\033[0m $1"; exit 1; }

# 1. Pre-clone RoboPlan
if [ -d "$ROBOPLAN_DIR/scripts" ]; then
    info "RoboPlan already cloned at .docker/roboplan/ — skipping clone."
    info "To re-clone, delete .docker/roboplan/ and re-run."
else
    info "Cloning RoboPlan (this takes a minute)..."
    rm -rf "$ROBOPLAN_DIR"
    mkdir -p "$REPO_DIR/.docker"
    git clone --depth 1 https://github.com/open-planning/roboplan.git "$ROBOPLAN_DIR"
    cd "$ROBOPLAN_DIR"
    git submodule update --init --recursive --depth 1
    cd "$REPO_DIR"

    info "Stripping .git directories..."
    find "$ROBOPLAN_DIR" -name ".git" -type d -exec rm -rf {} + 2>/dev/null || true
    find "$ROBOPLAN_DIR" -name ".git" -type f -delete 2>/dev/null || true

    info "RoboPlan ready ($(du -sh "$ROBOPLAN_DIR" | cut -f1))."
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
