# shellcheck shell=bash
#
# Source this file in every shell that launches a ROS 2 participant for the
# Isaac Lite6 trocar FoundationPose workflow:
#
#   source /home/tndlux/workspaces/dev_ws/src/manyforge/config/ros/isaac_lite6_trocar_foundationpose.env.sh
#
# Contexts:
# - Isaac Sim laptop shell before ./isaac-sim.sh
# - Isaac ROS 4.4 perception container on Thor/Jetson
# - ManyForge launcher shell before scripts/launch.sh
#
# Override the host IPs before sourcing when the topology changes:
#
#   export MANYFORGE_THOR_IP=192.168.1.136
#   export MANYFORGE_ISAAC_SIM_IP=192.168.1.132
#   source .../isaac_lite6_trocar_foundationpose.env.sh
#
# Override the ROS domain only when intentionally isolating another cell:
#
#   export MANYFORGE_ROS_DOMAIN_ID=7

unset FASTRTPS_DEFAULT_PROFILES_FILE
unset FASTDDS_DEFAULT_PROFILES_FILE
unset ROS_DISCOVERY_SERVER
unset ROS_SUPER_CLIENT
unset CYCLONEDDS_URI

export MANYFORGE_THOR_IP="${MANYFORGE_THOR_IP:-192.168.1.136}"
export MANYFORGE_ISAAC_SIM_IP="${MANYFORGE_ISAAC_SIM_IP:-192.168.1.132}"

export ROS_DOMAIN_ID="${MANYFORGE_ROS_DOMAIN_ID:-0}"
export RMW_IMPLEMENTATION="${MANYFORGE_RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
export ROS_AUTOMATIC_DISCOVERY_RANGE="${MANYFORGE_ROS_AUTOMATIC_DISCOVERY_RANGE:-LOCALHOST}"
export ROS_STATIC_PEERS="${MANYFORGE_ROS_STATIC_PEERS:-${MANYFORGE_THOR_IP};${MANYFORGE_ISAAC_SIM_IP}}"

return 0 2>/dev/null || true
