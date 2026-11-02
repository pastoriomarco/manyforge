#!/usr/bin/env bash
# Add pinned Noble ccache tooling to a local, immutable full Lyrical base image.
# Legacy filename retained for existing callers.
set -euo pipefail
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
if (( $# != 1 )); then
    echo "Usage: $0 LOCAL_BASE_IMAGE (MANYFORGE_DIAGNOSTIC_IMAGE selects output tag)" >&2
    exit 64
fi
base_id=$(docker image inspect --format '{{.Id}}' "$1")
base_alias="manyforge-ros-lyrical-cache-base:${base_id#sha256:}"
output=${MANYFORGE_DIAGNOSTIC_IMAGE:-manyforge-ros-lyrical-cache:local}
if alias_id=$(docker image inspect --format '{{.Id}}' "$base_alias" 2>/dev/null); then
    [[ "$alias_id" == "$base_id" ]] || {
        echo "Pinned base alias has a different image ID: $base_alias" >&2
        exit 69
    }
else
    docker image tag "$base_id" "$base_alias"
fi
echo "ROS diagnostic tooling base: $base_id"
docker build --pull=false --tag "$output" \
    --build-arg "MANYFORGE_ROS_BASE=$base_alias" \
    --build-arg "MANYFORGE_ROS_BASE_ID=$base_id" - < "$repo_root/Dockerfile.ros-lyrical-cache"
docker image inspect --format 'ROS diagnostic tooling image: {{.Id}}' "$output"
