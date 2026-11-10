# Real robot setup

How to connect ManyForge to a real UFACTORY xArm-family robot (xArm 5/6/7,
Lite 6, UF850) and run a first attended session. Lite 6 is the model measured
on hardware; physical execution is not yet qualified. The adapter's own
reference is the [xArm vendor page](../../manyforge_xarm_control/vendor/README.md).

## Prerequisites

- **Controller on and on the LAN.** The robot's control box is powered and
  reachable from the host that runs ManyForge (the workbench container uses
  the host network). Check from that host: `ping 192.168.1.31` (your robot's
  address).
- **Safety is the integrator's.** The e-stop and the safety chain are wired to
  the control box by the integrator. ManyForge is not safety-rated: its Stops
  are operational, not emergency stops.
- **Attended sessions only.** Someone stays at the e-stop whenever the motors
  are on.
- **The workbench.** Real robots run in the
  [workbench](DEVELOPMENT_WORKBENCH.md) (`./scripts/compose-dev.sh`); the image
  already contains the xArm driver.

## Site settings: the robot's address

A robot's IP address is a fact of the machine running ManyForge, not of the
Scenario. It lives in the site settings file, a JSON document:

```json
{
  "schema": "manyforge.site.v1",
  "device_addresses": { "lite6_main": "192.168.1.31" },
  "project_folders": ["/home/USER/Projects"]
}
```

- `schema` must be `manyforge.site.v1`; no other fields are accepted.
- `device_addresses` maps a key to a host name or IP address. Use lower-case
  keys (letters, digits, `.`, `_`, `-`, starting with a letter): the machine
  card accepts only those. Scenarios name the key, never the address.
- `project_folders` (optional) lists host folders shown in Composer's file
  pickers (see [Project folders](DEVELOPMENT_WORKBENCH.md#project-folders)).
- Keep the file anywhere on the host and set its **absolute** path before
  starting the workbench:

  ```bash
  export MANYFORGE_DEV_SITE_SETTINGS=/home/USER/manyforge.site.json
  ./scripts/compose-dev.sh start
  ```

- The file is read when the workbench starts. After changing it, run
  `./scripts/compose-dev.sh stop`, then `start` again. A missing file or an
  invalid one stops `start` with its reason
  ([Site file errors](TROUBLESHOOTING.md#site-file-errors)).

Example: [`docs/examples/site/manyforge.site.example.json`](../examples/site/manyforge.site.example.json).

## The machine in the Scenario

An xArm is one **machine**: an **owner** (the machine: driver process,
connection, controller errors, power) and its **members**: the arm, the
controller I/O and the tool I/O. In the Deployment the owner is a component
such as `ufactory.lite6_machine`; each member row names its `owner` and
`role` (`arm`, `controller_io`, `tool_io`).

On the **Components** page, the machine card of the owner holds:

- **Lane:** `real`. Every member runs on the machine's lane.
- **Address (site settings key):** the key in `device_addresses`, e.g.
  `lite6_main`. It is stored as the owner's `configuration.device_address_ref`.
  The real lane requires it; the other lanes don't take one.
- **Members**, each with an **Active** box. A deactivated member
  (`active: false`) stays in the file but isn't part of the running cell.
  - With the **arm deactivated** the machine is I/O-only: the owner still runs
    the driver (so the I/O stream flows), but the arm has no controllers, its
    hardware stays inactive and the motors are never enabled. The arm is not
    in the Scene.
  - Deactivate an I/O member you don't use; a component that uses one of its
    signals must be changed first.
- **Controller status** (once connected): the controller's `mode`, `state`,
  `error`, `warning` and `motors_enabled`, as reported, for diagnosis.

## What each action does on an xArm

Actions are on the Components page (and `POST /api/components/{id}/actions`).
A member action is refused (`owner_not_prepared`) until the machine is
prepared. Load and Apply leave the affected components **prepared**, never
enabled.

| Component | Action | What happens |
| --- | --- | --- |
| Machine | Prepare | If not connected (no recent status report) or after Power off / Reboot: restart the machine's driver process and wait for its first report, at most the component's lifecycle timeout. Then `clean_error`, read back three times 150 ms apart, and `clean_warn` if a warning is left. The motors are not switched on. A servo error (C10–C17) isn't cleared: power-cycle the robot. An arm taken over from UFACTORY Studio or the pendant is released. |
| Machine | Recover | As Prepare. |
| Machine | Power off | Members are disabled first, then the controller shuts down (`system_control(1)`). The machine shows offline ("shut down by the operator"), not a fault. Switch it on at the control box, then Prepare. The Composer asks for confirmation first. |
| Machine | Reboot | Members disabled first, `system_control(2)`, wait for the controller to go down and come back, then as Prepare. Offline ("rebooting") meanwhile. The Composer asks for confirmation first. |
| Arm | Prepare, Recover | **The motors stay off.** Wait for the arm controller to be loaded (Recover switches it off), keep the arm's hardware off, and wait for a recent status report. The Scene shows the real pose from the driver's joint topic. |
| Arm | Enable | **The motors switch on here.** The arm's hardware is switched on, which runs the driver's arming sequence `clean_error`, `clean_warn`, `motion_enable`, `set_mode(1)`, `set_state(0)` (also when the robot isn't ready after a takeover or an error: the hardware is switched off and on again); then fresh joint states. Then the check: a recent report with no error, mode 1, state ready or moving, all motors enabled, and every measured joint close to the controller's reported angle. Then the arm controller is switched on: the arm accepts motion. |
| Arm | Disable | Arm controller off, then hardware off. The arm stays prepared; Enable re-arms it. |
| I/O | Prepare, Enable, Disable | Needs the driver's I/O stream; Enable allows output writes. |

Controller errors become faults of the machine (owner), naming the members
they concern, except an e-stop (C1–C3), which is a stop that gates motion
only: C31 protective stop, C22 self-collision, C10–C17
drive, C19/C33 communication, and so on. A mode or state change made in Studio
or on the pendant while the arm is enabled is "taken over outside ManyForge".
While the machine isn't usable, its members show **offline** with
`owner_unavailable`.

**Errors stay until cleared.** After an e-stop is released the controller
keeps the error (C2 for this cell's wiring, the control box emergency input)
until `clean_error`. That is normal: Load and Prepare work with the e-stop
pressed and never clear it. Once it is released, **Enable all** (or Recover
the machine, then Enable the arm) clears it with the motors off. If the error
comes back, the e-stop is still active.

## First session checklist

1. Robot on, network checked (`ping`), e-stop released and within reach.
2. Site file with your robot's key (the examples use `lite6_main`);
   `MANYFORGE_DEV_SITE_SETTINGS` set; workbench (re)started.
3. **Load `examples/real_lite6_arm_observation.scenario`.** Arm only: both I/O
   members are deactivated, and the gripper is display-only (no output), so
   no gripper or wiring is needed. Load prepares the machine, then the arm:
   **the motors stay off**. A first session with the motors off is just
   Load (and Prepare if needed).
4. On the machine card, read **Controller status**: expect `error` none and
   the motors off (typically `state` 4). A leftover error (e.g. C2 after an
   e-stop) shows here and as the machine's fault: **Recover** the machine.
5. Check that the arm in the Scene matches the real pose (measured joints).
6. **Enable** the arm: **the motors switch on** and hold the pose (expect
   `mode` 1, `state` 2, `motors_enabled` 6); nothing moves. Then open
   **Robot Control** for attended, slow motion (jog), e-stop at hand.
7. To run a Program on the robot, press **Execute** (with the arm enabled, or
   **Activate and execute**), then **Run** on its motion group: every
   Program starts held. Stay at the e-stop.
8. Disable the arm when done; Power off the machine if the cell is closed.

Then the pneumatic examples (configure the wiring and polarity to your cell
first; the values in the files are illustrative):

| Example | Needs |
| --- | --- |
| `real_lite6_pneumatic_valve_only` | A valve on controller output **CO0** (`true` closes). Close completes after its dwell; no grasp sensor. Controller I/O active, tool I/O deactivated. |
| `real_lite6_pneumatic_pick_and_place` | The same valve on CO0, plus a grasp sensor published as a ROS `std_msgs/Bool` on `/workcell/gripper/workpiece_grasped`, periodically (a sample older than 0.5 s counts as stale). |

## Troubleshooting

| Shown | Meaning | Do |
| --- | --- | --- |
| `start` stops with a site settings message | The site file path or content is invalid. | See [Site file errors](TROUBLESHOOTING.md#site-file-errors). |
| `site_settings_unavailable` (Apply/Load fails, naming the component) | No site settings file is configured. | Set `MANYFORGE_DEV_SITE_SETTINGS` (absolute path), restart the workbench. |
| `site_device_address_ref_unknown` | The machine card's address key isn't in `device_addresses`. | Fix the key on the machine card, or add it to the site file and restart. |
| `xarm_status_unavailable` (Prepare fails), machine fault `fault_communication` | The controller sends no status report: unreachable, off, or the wrong address. | `ping` the address from the host; check power, cable, the address in the site file; then Prepare/Recover the machine. |
| `fault_process_exited` | The machine's driver process exited. | Prepare the machine (restarts it). |
| `robot_controller_unresponsive`, `robot_controller_call_failed` | A controller call got no answer, or was refused. | Check the controller and its network link, then Recover. |
| `fault_estop` (C1/C2/C3), `robot_error_not_cleared` | The e-stop is active, or the error stayed after `clean_error`. | Release the e-stop, Recover the machine, Enable the arm. |
| `fault_drive` (C10–C17) | Servo/drive error. | Power-cycle the robot at the control box, then Prepare. |
| `fault_external_control` | Studio or the pendant changed the mode/state. | Close the other client, Recover the machine, Enable the arm. |
| `xarm_not_ready`, `xarm_measured_joints_disagree`, `xarm_joint_angles_unavailable` (Enable fails) | The robot isn't ready, or measured and reported joints disagree. | Recover the machine, then Enable the arm again. |
| `owner_unavailable` on a member | The member is offline because its machine is offline, not prepared or in fault. | Look at the machine's state and fix that; the member follows. |
| `owner_not_prepared` | A member action before the machine is prepared. | Prepare the machine first. |
| `component_adapter_unknown` | No installed adapter for the model's lane row. | Check the model's `lanes.real` row; use a supported model. |

## For assistants (MCP/REST)

Checkable through the API (`manyforge_components` MCP tool, or REST):

- component states, including the machine's `details` (mode, state, error,
  warning, motors), faults with their codes, `owner_id`/`role` and
  `owner_unavailable` reasons: `GET /api/components`;
- the cell latch: `GET /api/cell/latch`; Execute/Resume readiness:
  `GET /api/program/readiness`;
- an Apply/Load result and its error code (e.g. `site_settings_unavailable`);
- component actions (Prepare, Recover, Enable, ...):
  `POST /api/components/{id}/actions`.

Ask the operator for: the site settings file and its key, restarting the
workbench, robot power and network (`ping`), the e-stop, and confirmation that
someone is attending before any Enable or motion.
