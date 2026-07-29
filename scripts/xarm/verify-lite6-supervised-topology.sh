#!/usr/bin/env bash
# Verify the deployed sole-owner ROS/container boundary before mutation.

set -euo pipefail

CONTAINER=${MANYFORGE_LITE6_CONTAINER:-manyforge-lite6-supervised}
RUNTIME_BINDING=${MANYFORGE_LITE6_RUNTIME_BINDING:-/tmp/manyforge-real-lite6/resolved/lite6-a/runtime-binding.json}
OWNER_LABEL=org.manyforge.xarm-transport-owner
OWNER_ID=lite6-a:sole-owner

if [[ "$(docker inspect "${CONTAINER}" --format '{{.State.Running}}' 2>/dev/null || true)" != "true" ]]; then
    echo "Lite6 supervised owner is not running: ${CONTAINER}" >&2
    exit 1
fi
if [[ "$(docker inspect "${CONTAINER}" --format '{{.HostConfig.NetworkMode}}')" != "host" ]] ||
   [[ "$(docker inspect "${CONTAINER}" --format '{{.HostConfig.IpcMode}}')" != "host" ]]; then
    echo "Supervised owner must use host network and host IPC" >&2
    exit 1
fi
if [[ "$(docker inspect "${CONTAINER}" --format "{{index .Config.Labels \"${OWNER_LABEL}\"}}")" != "${OWNER_ID}" ]]; then
    echo "Container does not own the exact Lite6 transport label" >&2
    exit 1
fi
mapfile -t LABELED_OWNERS < <(
    docker ps --filter "label=${OWNER_LABEL}=${OWNER_ID}" --format '{{.Names}}'
)
if ((${#LABELED_OWNERS[@]} != 1)) ||
   [[ "${LABELED_OWNERS[0]}" != "${CONTAINER}" ]]; then
    echo "Exactly one labeled Lite6 transport owner must be running" >&2
    exit 1
fi

docker exec \
    -e MANYFORGE_TOPOLOGY_RUNTIME_BINDING="${RUNTIME_BINDING}" \
    "${CONTAINER}" bash -lc '
      set -euo pipefail
      set +u
      source /opt/ros/jazzy/setup.bash
      source /opt/manyforge/xarm/install/setup.bash
      source /tmp/manyforge-build/real-lite6/install/setup.bash
      set -u

      mapfile -t expected < <(
        python3 - "${MANYFORGE_TOPOLOGY_RUNTIME_BINDING}" <<"PY"
import json
from pathlib import Path
import sys

binding = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
controller = binding["controller_binding"]
for key in (
    "supervised_state_topic",
    "supervised_execute_service",
    "supervised_command_gate_service",
    "supervised_shutdown_controller_service",
):
    print(controller[key])
PY
      )
      if ((${#expected[@]} != 4)); then
        echo "Runtime binding did not resolve the supervised surface" >&2
        exit 1
      fi

      topics=$(ros2 topic list -t --no-daemon --spin-time 2)
      services=$(ros2 service list -t --no-daemon --spin-time 2)
      grep -Fqx "${expected[0]} [xarm_msgs/msg/SupervisedLifecycleState]" \
        <<<"${topics}"
      grep -Fqx "${expected[1]} [xarm_msgs/srv/ExecuteSupervisedLifecycleCommand]" \
        <<<"${services}"
      grep -Fqx "${expected[2]} [xarm_msgs/srv/SetSupervisedCommandGate]" \
        <<<"${services}"
      grep -Fqx "${expected[3]} [xarm_msgs/srv/ShutdownSupervisedController]" \
        <<<"${services}"

      # The supervised ros2_control hardware retains the vendor internal
      # /ufactory_driver node name. Authority is distinguished by its exposed
      # service surface, not that implementation-detail node name.
      if grep -Eiq \
        "/(motion_ctrl|motion_enable|set_mode|set_state|clean_error|clean_warn|system_control|set_servo_angle|set_servo_cartesian)( |$)" \
        <<<"${services}"; then
        echo "A parallel legacy xArm command service is visible" >&2
        exit 1
      fi
    '

echo "Lite6 supervised sole-owner topology verified."
