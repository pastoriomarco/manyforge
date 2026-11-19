#!/usr/bin/env bash
# Input key of the reference ROS image (Dockerfile.ros-lyrical-full), shared by
# ros-image.yml (which publishes ghcr.io/<owner>/manyforge-ros:<key>) and
# ros-integration.yml (which runs the ROS suites in it).
#
# Usage:
#   scripts/ci/ros-image-inputs.sh               # prints inputs-<hash>
#   scripts/ci/ros-image-inputs.sh --build-args  # the image's build arguments
#   scripts/ci/ros-image-inputs.sh --paths       # the hashed paths
#
# The hash covers the committed content and mode of every file the image build
# reads (the Dockerfile, everything it COPYs or bind-mounts, setup-docker.sh's
# pinned .docker/ sources, the entrypoint) and the build arguments. Base images,
# apt packages and the upstream sources the Dockerfile pins by commit are not
# files here; a change to any of them is a Dockerfile change.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

build_args=(
    ROS_BASE=source
    ROBOPLAN_SOURCE_MODE=qualified
)
inputs=(
    .dockerignore
    Dockerfile.ros-lyrical-full
    LICENSE
    NOTICE
    THIRD_PARTY_NOTICES.md
    docker-entrypoint.ros-jazzy-full.sh
    scripts/ci/build-roboplan-source-closure.sh
    scripts/ci/build-roboplan-source-stack.sh
    scripts/ci/check-roboplan-dependency-stack.py
    scripts/ci/ros-image-inputs.sh
    scripts/ci/verify-external-source-closure.py
    scripts/setup-docker.sh
    third_party/external_delta_ledger.v1.json
    third_party/roboplan
    third_party/ros_lyrical
)

case "${1:-}" in
    "") ;;
    --build-args) printf '%s\n' "${build_args[@]}"; exit 0 ;;
    --paths) printf '%s\n' "${inputs[@]}"; exit 0 ;;
    *) echo "Usage: $0 [--build-args | --paths]" >&2; exit 64 ;;
esac

# `git ls-files -s`: mode, blob hash and path of each committed file.
hash=$({ printf '%s\n' "${build_args[@]}"; git ls-files -s -- "${inputs[@]}"; } \
    | sha256sum | cut -c1-16)
echo "inputs-${hash}"
