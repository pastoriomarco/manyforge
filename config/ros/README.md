# ROS Environment Profiles

This directory contains sourceable ROS 2 environment profiles for ManyForge
operator workflows.

## Isaac Lite6 Trocar FoundationPose

Use `isaac_lite6_trocar_foundationpose.env.sh` for the multi-machine workflow
where Isaac Sim runs on the laptop, Isaac ROS 4.4 YOLOv8/FoundationPose runs on
Thor or another Jetson, and ManyForge runs its ROS-capable launcher on the
Thor/Jetson host.

Source the same profile in every ROS participant shell:

```bash
source /home/tndlux/workspaces/dev_ws/src/manyforge/config/ros/isaac_lite6_trocar_foundationpose.env.sh
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
source /home/tndlux/workspaces/dev_ws/src/manyforge/config/ros/isaac_lite6_trocar_foundationpose.env.sh
```

The profile intentionally overwrites existing `ROS_DOMAIN_ID`,
`RMW_IMPLEMENTATION`, `ROS_AUTOMATIC_DISCOVERY_RANGE`, and `ROS_STATIC_PEERS`
so stale shell state does not leak into a commissioning session. Override with
`MANYFORGE_ROS_DOMAIN_ID`, `MANYFORGE_RMW_IMPLEMENTATION`,
`MANYFORGE_ROS_AUTOMATIC_DISCOVERY_RANGE`, or `MANYFORGE_ROS_STATIC_PEERS` only
when intentionally qualifying a different topology.

See `docs/operations/ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md` for the full
bring-up sequence, camera QoS settings, TF checks, and smoke validation.
