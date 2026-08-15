# ROS Environment Profiles

> **Current `dev` boundary:** `isaac_i9.env.sh` is the active I9 topology input
> for the source-matched mechanical Isaac/Jazzy product. The older
> FoundationPose profile remains a historical reference; it is not the I9
> topology API. Sourcing either file alone is not scenario qualification.

This directory contains sourceable ROS 2 environment profiles from the last
qualified ManyForge operator workflows.

## I9 laptop and Orin topology

Source `isaac_i9.env.sh` in the laptop Isaac shell and in every Orin ROS
participant/container shell:

```bash
MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
source "${MANYFORGE_ROOT}/config/ros/isaac_i9.env.sh"
```

Its recorded default is one role-free explicit peer list:

- `ROS_DOMAIN_ID=0`
- `RMW_IMPLEMENTATION=rmw_fastrtps_cpp`
- `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`
- `ROS_STATIC_PEERS=192.168.1.132;192.168.1.68`

The profile unsets stale Fast DDS discovery-server/profile and Cyclone DDS
inputs. A different domain or peer set is valid only when applied to every
participant as one recorded topology input. Use `MANYFORGE_ROS_DOMAIN_ID`,
`MANYFORGE_RMW_IMPLEMENTATION`,
`MANYFORGE_ROS_AUTOMATIC_DISCOVERY_RANGE`, and
`MANYFORGE_ROS_STATIC_PEERS` for that deliberate whole-topology override.

The Orin launcher derives `/manyforge/<controllable_component_id>` from
`MANYFORGE_ISAAC_COMPONENT_ID` and passes that same namespace to the retained
execution backend, controller manager, command gate, and gripper adapter. Raw
Isaac transport topics remain explicit launch inputs so a multi-world fixture
can assign disjoint wires without changing the retained single-UR10 defaults.

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
