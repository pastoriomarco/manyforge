# xArm vendor adapter

ManyForge drives every UFACTORY xArm-family robot (xArm 5/6/7, Lite 6, UF850) through one vendor adapter, `manyforge_xarm_control`, and the vendor's ROS 2 driver with its ros2_control hardware plugin. This page is the adapter's configuration: where the driver comes from, what ManyForge adds to the vendor's files and why, which robots are supported, and how to add one.

## Driver

| Item | Value |
| --- | --- |
| Source | `https://github.com/pastoriomarco/xarm_ros2`, branch `lyrical-no-gazebo` |
| Pinned commit | `driver.yaml` `source.commit` (the image build checks out this commit) |
| Hardware plugin | `uf_robot_hardware/UFRobotSystemHardware` |
| Base files | `xarm_controller/config/{model}_controllers.yaml`, `xarm_api/config/xarm_params.yaml` |

The fork adds, opt-in and in upstream style, the I/O stream `xarm_io_states` (message `xarm_msgs/IOStates`, parameters `io_states.enable`, `io_states.rate`, `io_states.tgpio_monitor_frequency`) and the service `system_control` (1 = shut down, 2 = reboot). Upstream behaviour is unchanged when they're unset.

The base files are read when a component's configuration is generated. No vendor launch file or Python runs.

## ManyForge's additions (`manyforge_settings.yaml`)

Every addition is in `manyforge_settings.yaml` with its reason. In short:

- **Hardware:** `report_type: rich`, because the GPIO module's health and pin configuration (`xarm_cgpio_states`) come only with the rich report. The plugin's `prefix`/`robot_ip` take a leading character it drops (`P`, `R`). The hardware starts inactive: the arm's Enable arms it (Prepare leaves the motors off).
- **Driver services:** only the listed ones are on, every other service of the vendor file is off. The motion services stay off: ManyForge is the only motion authority.
- **I/O stream:** `io_states.enable: true`, published at 100 Hz, tool monitoring at 100 Hz.
- **Controllers:** the vendor's controller file plus Stop deceleration (`decelerate_on_cancel`, each joint's bound from the Deployment's Stop profile), each joint's `goal` set to the model's arrival tolerance, the inactive hardware start, and ManyForge's goal monitoring settings.
- **Remaps:** the plugin calls absolute controller-manager and RViz names; they're sent to the component's own namespace.

## What the adapter does

An xArm is one machine (`kind: machine`, e.g. `ufactory.lite6_machine`) with an owner and members: the arm, the controller I/O and the tool I/O. The owner holds what is machine-wide (the driver process, the connection, the controller's error register, power); members are refused while it isn't prepared.

| Component | Action | xArm sequence |
| --- | --- | --- |
| Owner | Prepare | When it isn't connected (no fresh status report) or after Power off / Reboot: restart the machine's process and wait for its first report. The wait for the first report is bounded by the component's lifecycle command timeout. Then `clean_error`, read back three times 150 ms apart, and `clean_warn` when a warning is left; never `motion_enable`. An error that stays (an e-stop still pressed) fails the action: "Couldn't clear the robot error. If the e-stop is pressed, release it, then Recover the machine and Enable the arm." A servo error (C10-C17) isn't cleared: power cycle the robot. An arm taken over from outside is released; its Enable re-arms it. |
| Owner | Recover | As Prepare (it reconnects a lost driver the same way). |
| Owner | Power off | Disable the members, then `system_control(1)`. The machine is offline ("shut down by the operator"), not in fault; switch it on at the control box, then Prepare. |
| Owner | Reboot | Disable the members, `system_control(2)`, wait for the controller (at most `reboot.controller_wait_s`, probing its command port every `reboot.probe_interval_ms`), restart the process, `clean_error`. Offline ("rebooting") meanwhile. |
| Arm | Prepare, Recover | The motors stay off. Wait for the loaded arm controller (Recover switches it off), switch the hardware off (it already is on a fresh process), then wait for a fresh status report, at most the component's lifecycle command timeout. The owner (prepared first) cleared the error and warning. While prepared, the driver's own `joint_states` (published with the hardware off, the true pose even during an e-stop) feeds the arm's measured joints and live view. |
| Arm | Enable | The motors are switched on here. With the hardware off, or the robot not ready for commands (after an error, an e-stop released, taken over from outside): the arm controller off, the hardware off (when on), then on: the plugin's arming sequence (`clean_error`, `clean_warn`, `motion_enable`, `set_mode(1)`, `set_state(0)`) with the joint state broadcaster; then a broadcaster sample stamped after it, at most the component's lifecycle command timeout. Then the Enable gate: a fresh report that accepts commands and every measured joint within `joint_agreement_tolerance_rad` of the controller's reported angle (joints during an e-stop and the plugin's zero fallback aren't a pose). Then the arm controller is switched on. |
| Arm | Disable | The arm controller is switched off, then the hardware (the plugin selects position mode). The arm stays prepared: Enable re-arms it. |
| I/O | Prepare, Enable, Disable | Needs a fresh stream; Enable allows writes. |

**Owner state:** controller errors become ManyForge faults of the owner (C1-C3 e-stop, C31 protective stop, C22 self-collision, C21/C25 invalid motion, C23/C24/C35/C60 limit, C10-C17 drive, C19/C33 communication, anything else the vendor fault with the code and text), each naming the members it concerns: C33 and the GPIO module's errors (communication) the controller I/O, C19 the tool I/O, the arm's own errors (servo, motion, collision, limits) the arm, anything else (e-stops included) every member. A missing status report is a lost connection of the whole machine. A mode or state change from UFACTORY Studio or the pendant while ManyForge has the arm armed (out of joint-servo mode, or paused, stopped, not ready or stop requested) is "taken over outside ManyForge", concerning the arm. Recover on the owner clears them; then Enable the arm. The owner's details show the report's mode, state, error, warning and motors enabled. The I/O stream is stale after ten publish periods (`io_states.rate`).

**Arm state:** ManyForge's own operations explain the mode and state changes they cause until a report shows the robot armed again, at most 5 s after they end; a controller error explains its own and disarms the arm. Samples of the driver's own `joint_states` that are all zero (the e-stop signature) are never a pose. With all motors off the arm is idle.

**Always a machine:** a Lite 6 or UF850 arm is placed only as the arm member of its machine (`ufactory.lite6_machine`, `ufactory.uf850_machine`), on every lane. Members are never removed, only deactivated (`active: false` on the member row), and every member may be deactivated, the arm included. The machine's process belongs to the owner: with the arm deactivated, the owner still runs the driver (ros2_control with the arm's description, no arm controller, hardware left inactive), so the I/O stream flows and the motors are never enabled; the deactivated arm isn't in the cell (no Scene, TF or routes). On the Mock and Isaac lanes the machine model has no row: the owner is the host's passive owner and the I/O members use the host's Mock I/O.

**Address:** the machine's Deployment row names a site-settings key, never an address: `configuration.device_address_ref` (e.g. `lite6_main`) resolves in the site settings' `device_addresses` (`{"schema": "manyforge.site.v1", "device_addresses": {"lite6_main": "192.168.1.31"}}`), which fills `${address}` in `robot_ip`.

**After an e-stop:** a controller error left after an e-stop (e.g. C2, the control box's emergency input) is normal and stays until it's cleared. Release the e-stop, Recover the machine (the owner's `clean_error` clears it), then Enable the arm. While the e-stop is still pressed, Prepare and Recover fail with "Couldn't clear the robot error".

**I/O:** each I/O member offers the signals of its model (`io_device`, `lanes.real: {adapter: xarm, bank: controller | tool}`), read from `xarm_io_states`, which the owner subscribes to once.

- A write that the controller accepts is shown at once. `ret=1` (a controller error is present) is accepted too: the write takes effect, and the error is the owner's fault. If the stream doesn't show it within the confirmation bound, 2 × (publish period + tool monitoring period) + one publish period (50 ms with the settings above), or the value later changes in the stream, it's shown as changed outside ManyForge.
- A write with no answer is unknown, never resent; the stream's next report of that output settles it.
- A controller output with a controller function (Studio's output function other than general output) is read-only.
- Only the inputs and outputs the model declares are offered: the controller accepts writes to outputs that don't exist.
- A value lasting less than about one publish period (10 ms) can be missed, inputs included.
- A stopped stream faults the I/O member while its owner is prepared and connected; otherwise the member shows offline through its owner.

## Supported robots

| Model | Machine (`components/machine/`) | Arm `component.yaml` | Vendor model | Notes |
| --- | --- | --- | --- | --- |
| UFACTORY Lite 6 | `ufactory.lite6_machine` | `manyforge_example_models/models/lite6` | `lite6` | measured on the robot |
| UFACTORY 850 | `ufactory.uf850_machine` | `manyforge_example_models/models/uf850` | `uf850` | I/O list to verify on a robot |

xArm 5/6/7 use the same adapter and settings; they need only their model folders (below).

## Adding a robot

A robot of this family is model folders, no code: the arm model (e.g. `manyforge_example_models/models/lite6`), its I/O bank models (`components/io_device/ufactory.lite6_controller_io`, `ufactory.lite6_tool_io`) and the machine model that lists them as members (`components/machine/ufactory.lite6_machine`). The arm's `component.yaml` gets a `lanes.real` row:

```yaml
lanes:
  real:
    adapter: xarm
    controller: xarm6_traj_controller   # the vendor controller file's arm controller
    arrival_tolerance_rad: 0.01
    vendor_model: xarm6                 # xarm_controller/config/xarm6_controllers.yaml
    robot_type: xarm
    dof: 6
    joint_agreement_tolerance_rad: 0.02 # Enable gate
```

Each I/O bank is an `io_device` model with its signals (only what the robot has) and `lanes.real: {adapter: xarm, bank: controller, input_maximum_age_ms: 100}` (or `tool`; the inputs' freshness budget, ten stream periods at 100 Hz). Signal names give the bit: `CIn`/`COn` are bits 0-7 of the controller word, `DIn`/`DOn` bits 8-15, `TIn`/`TOn` bits 0-4 of the tool word. The machine model (`kind: machine`) lists the members (`arm`, `controller_io`, `tool_io`) and has `lanes.real: {adapter: xarm}`. A deliberate per-model controller file replaces the vendor's with `controllers_file:` (relative to the model folder). The model's Mock and Isaac rows name `vendor: xarm` and its `vendor_model`, so they run these same controllers on their own hardware (the arm claims the hardware's command interfaces; only the real hardware starts inactive). Check each folder with `scripts/manyforge component check <folder>`.
