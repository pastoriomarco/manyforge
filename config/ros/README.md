# ROS Environment Profiles

Sourceable ROS 2 environment profiles for shells that take part in a
multi-machine cell (the Isaac Sim shell, a perception container, the shell that
runs `./scripts/compose-dev.sh start`). Sourcing one is optional.

The workbench itself needs none of them: `./scripts/compose-dev.sh start`
passes `ROS_DOMAIN_ID` (default `0`), `RMW_IMPLEMENTATION` (default
`rmw_fastrtps_cpp`), `ROS_AUTOMATIC_DISCOVERY_RANGE`, `ROS_STATIC_PEERS` and the
DDS profile variables through from its own environment when they are set, and
never picks peers itself. With `ROS_AUTOMATIC_DISCOVERY_RANGE` unset ROS uses
`SUBNET`, which reaches Isaac Sim and other ROS machines on the LAN;
`LOCALHOST` isolates a machine.

Both profiles unset stale Fast DDS discovery-server/profile and Cyclone DDS
inputs, then set `ROS_DOMAIN_ID`, `RMW_IMPLEMENTATION`,
`ROS_AUTOMATIC_DISCOVERY_RANGE` and `ROS_STATIC_PEERS`. Override each with its
`MANYFORGE_`-prefixed counterpart (`MANYFORGE_ROS_DOMAIN_ID`,
`MANYFORGE_RMW_IMPLEMENTATION`, `MANYFORGE_ROS_AUTOMATIC_DISCOVERY_RANGE`,
`MANYFORGE_ROS_STATIC_PEERS`), identically in every participant shell.

## `isaac_i9.env.sh`: explicit peer list

```bash
MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
source "${MANYFORGE_ROOT}/config/ros/isaac_i9.env.sh"
```

Defaults (an already set standard variable is kept):

- `ROS_DOMAIN_ID=0`
- `RMW_IMPLEMENTATION=rmw_fastrtps_cpp`
- `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`
- `ROS_STATIC_PEERS=192.168.1.132;192.168.1.68`

Set the peers to your machines' addresses before sourcing.

## `isaac_lite6_trocar_foundationpose.env.sh`: Isaac Sim + Isaac ROS perception

For Isaac Sim on one machine, Isaac ROS YOLOv8/FoundationPose on a Jetson, and
ManyForge on that Jetson. Override the host addresses before sourcing:

```bash
export MANYFORGE_THOR_IP=<jetson-ip>            # default 192.168.1.136
export MANYFORGE_ISAAC_SIM_IP=<isaac-sim-ip>    # default 192.168.1.132
MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
source "${MANYFORGE_ROOT}/config/ros/isaac_lite6_trocar_foundationpose.env.sh"
```

It sets `ROS_DOMAIN_ID=0`, Fast DDS, `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`
and `ROS_STATIC_PEERS=<jetson>;<isaac-sim>`, overwriting what the shell had
(only the `MANYFORGE_` overrides change them). See
`docs/operations/ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md` for that workflow.
