# ManyForge xArm control

`manyforge_xarm_control` is the xArm vendor adapter (CONTRACTS §6, §7, §12):
every xArm-family robot (xArm 5/6/7, Lite 6, UF850) through the driver of
`vendor/driver.yaml`. Its configuration and behaviour are documented on the
vendor page, [vendor/README.md](vendor/README.md).

## Parts

An xArm is one machine of several components (CONTRACTS §16): the owner
(the machine) and its members (the arm, the controller I/O, the tool I/O).

- `XArmOwnerAdapter` (`IComponentAdapter`): the owner. It holds the driver
  services, the controller's report (`robot_states`) and the single
  `xarm_io_states` subscription (with `xarm_cgpio_states`), and runs the
  machine-wide steps: the process restart, `clean_error` and `clean_warn`
  (Prepare, Recover, motors left off), the capabilities `power_off` and
  `reboot` through `system_control` (members disabled first). Controller
  errors, a lost report, the GPIO module's errors and external control of
  the arm are its faults (`xarm_fault`), each naming the members it concerns.
- `XArmArmAdapter` (`IComponentAdapter`): the arm member, around the
  ros2_control lane adapter: prepared with the hardware off (motors off,
  the driver's joint topic as measured joints); Enable activates the
  hardware (the motors switch on only here), passes the Enable gate, then
  activates the arm controller; Disable switches the controller, then the
  hardware off. It also applies the external-control rule the owner reports,
  is idle with all motors off, and sends the Active TCP's offset and the
  payload (`ApplyToolContext`, `XArmToolContextSender`).
- `XArmIoComponent` (`IComponentAdapter`, output writer): an I/O member,
  control-box I/O (`controller_io`) or tool I/O (`tool_io`), on the fork's
  `xarm_io_states` stream (`XArmIoStatesTransport`, held by the owner), with
  the pin configuration from `xarm_cgpio_states`. Its signals are
  `<member_id>/<signal_name>`, e.g. `lite6_robot_controller_io/CO0`.
- `XArmDriverServices` (`IXArmDriver`): the driver services the adapters call,
  each request once, never retried.
- `make_xarm_launch_contribution`: the launch contribution of a model's
  `lanes.real` row from the vendor folder (CONTRACTS §12).
- `make_xarm_machine`: one machine's owner adapter and its subscriptions.
- The adapter family `xarm` (`src/xarm_adapter_family.cpp`), which the
  execution host loads only through the `manyforge_component_adapters`
  index (CONTRACTS §6): one factory call per component, by role (the owner
  returns the handle its members are made on), the launch contribution, the
  vendor controllers for the model's Mock and Isaac rows (`vendor: xarm`) and
  the arm's tool-context hook; it takes the host's measured joints,
  driver-restart entry and I/O update sink. The I/O members take manual and
  Program writes through the output-writer interface.

## Tests

`test_xarm_fault` (fault catalogue and concerns), `test_xarm_owner_adapter`
(the owner's actions, faults and concerns, power actions, the I/O stream
hand-off), `test_xarm_arm_adapter` (the arm's action
sequences and state against a fake driver and lane), `test_xarm_io_component`
(writes, `ret=1`, unknown writes settled by the stream, confirmation, outside
changes, controller-owned and declared-only outputs, the bound, health),
`test_xarm_launch_contribution` (every supported model's configuration against
the vendor settings, I/O rows), `test_xarm_robot_states_transport`,
`test_xarm_driver_services`, `test_xarm_tool_context` and
`test_xarm_adapter_family` (the family from a test index, by role).
