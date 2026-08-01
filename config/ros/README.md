# ROS Environment Profiles

> **Current `dev` boundary:** these profiles are retained configuration and
> historical topology references. Sourcing one does not make an Isaac/ROS
> product lane available or qualified. Do not execute the linked bring-up
> recipes until I9/I restores the Isaac lane and its prerequisite headless
> gates.

This directory contains sourceable ROS 2 environment profiles from the last
qualified ManyForge operator workflows.

## Isaac Lite6 Trocar FoundationPose

The historical multi-machine workflow used
`isaac_lite6_trocar_foundationpose.env.sh`
where Isaac Sim runs on the laptop, Isaac ROS 4.4 YOLOv8/FoundationPose runs on
Thor or another Jetson, and ManyForge runs its ROS-capable launcher on the
Thor/Jetson host.

Its captured setup sourced the same profile in every ROS participant shell:

```bash
MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
source "${MANYFORGE_ROOT}/config/ros/isaac_lite6_trocar_foundationpose.env.sh"
```

The default topology is:

- Thor / ManyForge / Isaac ROS host: `192.168.1.136`
- Isaac Sim laptop: `192.168.1.132`
- ROS domain: `0`
- RMW: Fast DDS (`rmw_fastrtps_cpp`)
- Discovery: `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST` with explicit
  `ROS_STATIC_PEERS`

For a different LAN, override the host IPs before sourcing:

```bash
export MANYFORGE_THOR_IP=<thor-or-jetson-ip>
export MANYFORGE_ISAAC_SIM_IP=<isaac-sim-laptop-ip>
MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
source "${MANYFORGE_ROOT}/config/ros/isaac_lite6_trocar_foundationpose.env.sh"
```

The profile intentionally overwrites existing `ROS_DOMAIN_ID`,
`RMW_IMPLEMENTATION`, `ROS_AUTOMATIC_DISCOVERY_RANGE`, and `ROS_STATIC_PEERS`
so stale shell state does not leak into a commissioning session. Override with
`MANYFORGE_ROS_DOMAIN_ID`, `MANYFORGE_RMW_IMPLEMENTATION`,
`MANYFORGE_ROS_AUTOMATIC_DISCOVERY_RANGE`, or `MANYFORGE_ROS_STATIC_PEERS` only
when intentionally qualifying a different topology.

See `docs/operations/ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md` for the retained
bring-up sequence, camera QoS settings, TF checks, and future smoke-acceptance
requirements. It is not a current command sequence.
