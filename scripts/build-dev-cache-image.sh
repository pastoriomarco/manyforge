#!/usr/bin/env bash
# Add only pinned compiler-cache tooling to an existing ROS-free dev image.
set -euo pipefail
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
base=${MANYFORGE_DEV_IMAGE:-manyforge-dev:latest}
output=${MANYFORGE_DIAGNOSTIC_IMAGE:-manyforge-dev-cache:local}
if (( $# != 0 )); then
    echo "Usage: $0 (MANYFORGE_DEV_IMAGE selects the local base image)" >&2
    exit 64
fi
base_id=$(docker image inspect --format '{{.Id}}' "$base")
base_alias="manyforge-dev-cache-base:${base_id#sha256:}"
if alias_id=$(docker image inspect --format '{{.Id}}' "$base_alias" 2>/dev/null); then
    [[ "$alias_id" == "$base_id" ]] || {
        echo "Pinned base alias has a different image ID: $base_alias" >&2
        exit 69
    }
else
    docker image tag "$base_id" "$base_alias"
fi
echo "Diagnostic tooling base: $base_id"
# Stdin Dockerfile has no source context: no .docker setup or dependency build.
docker build --pull=false --tag "$output" \
    --build-arg "MANYFORGE_DEV_BASE=$base_alias" \
    --build-arg "MANYFORGE_DEV_BASE_ID=$base_id" - < "$repo_root/Dockerfile.dev-cache"
docker image inspect --format 'Diagnostic tooling image: {{.Id}}' "$output"
