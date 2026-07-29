#!/usr/bin/env bash
#
# Build the experimental real-Lite6 image from one exact local xarm_ros2 tree.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
XARM_SOURCE=${MANYFORGE_XARM_SOURCE_CONTEXT:-}
IMAGE_NAME=${MANYFORGE_REAL_LITE6_IMAGE:-manyforge-ros-jazzy-real-lite6:experimental}
BASE_IMAGE=${MANYFORGE_ROS_FULL_BASE_IMAGE:-manyforge-ros-jazzy-full}
XC4_BASE_COMMIT=50064477f39ecd69ab006488630c837cdcb8d83e
XC4_BASE_TREE=5b7ab25ecf7f5d030b891824f2cd308ea72c3a57
SUPERVISED_CONTRACT_MIN_COMMIT=b951ad1f4ce7f1bceb077c74778327f2155e42b7
SUPERVISED_CONTRACT_VERSION=1

if [[ -z "${XARM_SOURCE}" ]]; then
    echo "MANYFORGE_XARM_SOURCE_CONTEXT must name the local xarm_ros2 checkout." >&2
    exit 2
fi
XARM_SOURCE=$(cd "${XARM_SOURCE}" && pwd)
if [[ ! -d "${XARM_SOURCE}/.git" ]]; then
    echo "xarm source is not a Git worktree: ${XARM_SOURCE}" >&2
    exit 2
fi
if ! git -C "${XARM_SOURCE}" merge-base --is-ancestor \
    "${XC4_BASE_COMMIT}" HEAD; then
    echo "xarm source is not descended from the qualified XC-4 candidate." >&2
    exit 2
fi
if ! git -C "${XARM_SOURCE}" merge-base --is-ancestor \
    "${SUPERVISED_CONTRACT_MIN_COMMIT}" HEAD; then
    echo "xarm source lacks the required supervised ROS lifecycle contract." >&2
    echo "minimum commit: ${SUPERVISED_CONTRACT_MIN_COMMIT}" >&2
    exit 2
fi
if [[ ! -f "${XARM_SOURCE}/xarm_sdk/package.xml" ]] ||
   [[ ! -f "${XARM_SOURCE}/xarm_sdk/cxx/include/xarm/wrapper/xarm_api.h" ]]; then
    echo "xarm_sdk/cxx is not initialized in ${XARM_SOURCE}." >&2
    exit 2
fi

SOURCE_BRANCH=$(git -C "${XARM_SOURCE}" branch --show-current)
SOURCE_COMMIT=$(git -C "${XARM_SOURCE}" rev-parse HEAD)
SOURCE_TREE=$(git -C "${XARM_SOURCE}" rev-parse HEAD^{tree})
if [[ -n "$(git -C "${XARM_SOURCE}" status --porcelain=v1 --untracked-files=all)" ]]; then
    echo "xarm source must be clean before building a physical-test image." >&2
    exit 2
fi
SOURCE_DIRTY=false
SOURCE_CONTENT_SHA256=$(
    cd "${XARM_SOURCE}"
    find . \
        \( -name .git -o -name build -o -name install -o -name log \
           -o -name __pycache__ -o -name .pytest_cache \) -prune \
        -o -type f -print0 \
        | LC_ALL=C sort -z \
        | xargs -0 sha256sum \
        | sha256sum \
        | awk '{print $1}'
)
BASE_IMAGE_ID=$(docker image inspect "${BASE_IMAGE}" --format '{{.Id}}')

docker buildx build \
    --load \
    --build-context "xarm_ros2_source=${XARM_SOURCE}" \
    --file "${REPO_ROOT}/Dockerfile.ros-jazzy-real-lite6" \
    --tag "${IMAGE_NAME}" \
    --build-arg "MANYFORGE_ROS_FULL_BASE_IMAGE=${BASE_IMAGE}" \
    --build-arg "MANYFORGE_ROS_FULL_BASE_IMAGE_ID=${BASE_IMAGE_ID}" \
    --build-arg "XARM_SOURCE_BRANCH=${SOURCE_BRANCH}" \
    --build-arg "XARM_SOURCE_COMMIT=${SOURCE_COMMIT}" \
    --build-arg "XARM_SOURCE_TREE=${SOURCE_TREE}" \
    --build-arg "XARM_SOURCE_CONTENT_SHA256=${SOURCE_CONTENT_SHA256}" \
    --build-arg "XARM_SOURCE_DIRTY=${SOURCE_DIRTY}" \
    --build-arg "XARM_XC4_BASE_COMMIT=${XC4_BASE_COMMIT}" \
    --build-arg "XARM_XC4_BASE_TREE=${XC4_BASE_TREE}" \
    --build-arg "XARM_SUPERVISED_CONTRACT_VERSION=${SUPERVISED_CONTRACT_VERSION}" \
    "${REPO_ROOT}"

EVIDENCE_DIR="${REPO_ROOT}/.manyforge-var/real-lite6"
mkdir -p "${EVIDENCE_DIR}"
IMAGE_ID=$(docker image inspect "${IMAGE_NAME}" --format '{{.Id}}')
SOURCE_MANIFEST_SHA256=$(
    docker run --rm --network none "${IMAGE_NAME}" \
        sha256sum /opt/manyforge/xarm/source-manifest.json \
        | awk '{print $1}'
)
printf '%s\n' \
    '{' \
    '  "schema": "manyforge.real_lite6_image.v0.experimental",' \
    "  \"image\": \"${IMAGE_NAME}\"," \
    "  \"image_id\": \"${IMAGE_ID}\"," \
    "  \"base_image\": \"${BASE_IMAGE}\"," \
    "  \"base_image_id\": \"${BASE_IMAGE_ID}\"," \
    "  \"source_manifest_sha256\": \"${SOURCE_MANIFEST_SHA256}\"," \
    '  "qualification": "local_experimental"' \
    '}' \
    > "${EVIDENCE_DIR}/image-manifest.json"

echo "Built ${IMAGE_NAME}"
echo "Source commit/tree: ${SOURCE_COMMIT} / ${SOURCE_TREE}"
echo "Source content: ${SOURCE_CONTENT_SHA256} (dirty=${SOURCE_DIRTY})"
echo "Image ID: ${IMAGE_ID}"
