# shellcheck shell=bash
#
# Recorded, topology-neutral ROS 2 input for every I9 participant. Source this
# file in the laptop Isaac shell and every Orin runtime/container shell. The
# peer list is explicit and role-free: a participant never infers peers from a
# Thor, simulator, Manager, or worker host variable.
#
# To qualify a different whole-cell topology, set the standard ROS variables
# (or their MANYFORGE_* counterparts) identically for every participant before
# sourcing this file and record that input with the evidence batch.

unset FASTRTPS_DEFAULT_PROFILES_FILE
unset FASTDDS_DEFAULT_PROFILES_FILE
unset ROS_DISCOVERY_SERVER
unset ROS_SUPER_CLIENT
unset CYCLONEDDS_URI

export ROS_DOMAIN_ID="${MANYFORGE_ROS_DOMAIN_ID:-${ROS_DOMAIN_ID:-0}}"
export RMW_IMPLEMENTATION="${MANYFORGE_RMW_IMPLEMENTATION:-${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}}"
export ROS_AUTOMATIC_DISCOVERY_RANGE="${MANYFORGE_ROS_AUTOMATIC_DISCOVERY_RANGE:-${ROS_AUTOMATIC_DISCOVERY_RANGE:-LOCALHOST}}"
export ROS_STATIC_PEERS="${MANYFORGE_ROS_STATIC_PEERS:-${ROS_STATIC_PEERS:-192.168.1.132;192.168.1.68}}"

return 0 2>/dev/null || true
