# Mock lane

The Mock lane runs a component on the generic ros2_control path with stock
mock hardware instead of a device: the same generated `robot_description`,
controllers file and `ros2_control.launch.py` as Isaac and Real, with
`mock_components/GenericSystem` as the hardware.

## What the lane adds

| Setting | Value | Why |
| --- | --- | --- |
| Hardware plugin | `mock_components/GenericSystem` | stock ros2_control; mirrors commands into state |
| `calculate_dynamics` | `true` | velocity and acceleration states follow the commanded positions |
| Joint command interfaces | `position` | the arm's JTC claims position (a vendor's controllers claim the hardware's interfaces) |
| Initial state | the Deployment's initial joint positions | where the robot starts on each Apply |
| Reset-position writer | stock `ForwardCommandController`, inactive | serves the `reset_position` action |

The values are in `manyforge_settings.yaml`; the adapter's test checks them.

## A robot or tool on Mock

A model's `component.yaml` declares the Mock lane under `lanes.mock`. A model
of a vendor with an adapter (xArm) runs that vendor's controllers, as on the
real lane: its base files plus ManyForge's additions, on the mock hardware:

```yaml
lanes:
  mock:
    adapter: mock
    vendor: xarm                          # the vendor adapter whose controllers run here
    vendor_model: lite6                   # xarm_controller/config/lite6_controllers.yaml
    controller: lite6_traj_controller     # the arm's JTC
    arrival_tolerance_rad: 0.01           # every arm joint's JTC goal
    runtime_scale: supported              # the arm's JTC claims position only
    hard_stop_completion_timeout_s: 1.5
```

A model without a vendor adapter (UR, the tools) names its own
`controllers_file` (in the model folder) instead. It is a controllers template:

- one `/**/controller_manager:` block (the component's manager);
- `__controller__` where the row's controller is named;
- an arm's `joints: [__joint__]` and one
  `__joint__: {..., max_deceleration_on_cancel: null}` line, which becomes one
  line per arm joint with the row's arrival tolerance as `goal` and the
  Deployment's operator Stop deceleration;
- a tool's `__gripper_joint__`.

### A tool's Stop finish tolerance

A tool row also carries
`gripper_stop_finish_tolerance` (the gripper's position unit). On a Stop the
gripper command backend does not cancel when the finger position is already
within this tolerance of the commanded target; it lets the goal finish and
settles on the controller's result. Further away it cancels, and the
controller's acceptance of the cancel ends the command: the stock
`parallel_gripper_action_controller/GripperActionController` holds the current position before it accepts, and a
cancel that arrives after it finished the goal but before `realtime_tools`
sent the success result is accepted and never answered (no result follows).
The tolerance is wide on purpose (about 5% of the stroke) so the Stop
decides before the controller's own tight `goal_tolerance` is met. A
Deployment gripper row may override it with `stop_finish_tolerance`.

## State and actions

- Lifecycle: the controller manager unreachable is `offline`; the arm
  controller loaded inactive on active hardware is `prepared`; active is
  `enabled`; an exited launch is a fault.
- Activity: `busy` while the controller reports a goal running, or after a
  cancel while its reference still moves; `idle` otherwise.
- Actions: `prepare`, `enable`, `disable`, `recover`, and `reset_position`.
