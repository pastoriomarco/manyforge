#!/usr/bin/env bash
# Local commissioning ingress for one supervised Lite6 owner.
#
# This is intentionally a host-local, attended shell boundary. It reuses the
# generic LocalOperatorGateway and retains one evidence record per mutation.
# It is not the later credential-authenticated Composer ingress.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
COMMAND=${1:-}
ACTION=${2:-}
CONTAINER=${MANYFORGE_LITE6_CONTAINER:-manyforge-lite6-supervised}
LOCAL_OPT_IN=${MANYFORGE_ENABLE_UNAUTHENTICATED_LOCAL_OPERATOR:-}
ACTION_CONFIRMATION=${MANYFORGE_LITE6_ACTION_CONFIRMATION:-}
OWNER_LABEL=org.manyforge.xarm-transport-owner
OWNER_ID=lite6-a:sole-owner

usage() {
    echo "Usage: $0 status | execute <action-id>" >&2
}

if [[ "${COMMAND}" != "status" && "${COMMAND}" != "execute" ]]; then
    usage
    exit 2
fi
if [[ "$(docker inspect "${CONTAINER}" --format '{{.State.Running}}' 2>/dev/null || true)" != "true" ]]; then
    echo "Lite6 supervised owner is not running: ${CONTAINER}" >&2
    exit 2
fi
if [[ "$(docker inspect "${CONTAINER}" --format '{{.HostConfig.NetworkMode}}')" != "host" ]] ||
   [[ "$(docker inspect "${CONTAINER}" --format '{{.HostConfig.IpcMode}}')" != "host" ]] ||
   [[ "$(docker inspect "${CONTAINER}" --format "{{index .Config.Labels \"${OWNER_LABEL}\"}}")" != "${OWNER_ID}" ]]; then
    echo "Container is not the exact host-network/host-IPC supervised owner" >&2
    exit 2
fi

if [[ "${COMMAND}" == "status" ]]; then
    exec docker exec "${CONTAINER}" bash -lc '
      set -euo pipefail
      set +u
      source /opt/ros/jazzy/setup.bash
      source /opt/manyforge/xarm/install/setup.bash
      source /tmp/manyforge-build/real-lite6/install/setup.bash
      set -u
      MANYFORGE_SKIP_REAL_LITE6_BUILD=1 \
        /workspace/scripts/ros/run-lite6-supervised-preflight.sh
    '
fi

if [[ -z "${ACTION}" ]]; then
    usage
    exit 2
fi
if [[ "${LOCAL_OPT_IN}" != "I_ACCEPT_LOCAL_COMMISSIONING_RISK" ]]; then
    echo "Set MANYFORGE_ENABLE_UNAUTHENTICATED_LOCAL_OPERATOR=I_ACCEPT_LOCAL_COMMISSIONING_RISK" >&2
    exit 2
fi
if [[ "${ACTION_CONFIRMATION}" != "${ACTION}" ]]; then
    echo "MANYFORGE_LITE6_ACTION_CONFIRMATION must exactly match ${ACTION}" >&2
    exit 2
fi

export MANYFORGE_LITE6_ACTION="${ACTION}"
export MANYFORGE_LITE6_PRINCIPAL_ID="local-uid-$(id -u)"
export MANYFORGE_LITE6_AUTHENTICATION_CONTEXT_ID="attended-local-shell"
exec "${SCRIPT_DIR}/capture-lite6-supervised-command.sh"
