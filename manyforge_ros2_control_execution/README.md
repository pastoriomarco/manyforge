# ManyForge ros2_control execution

> **Current `dev` boundary:** this retained C++ adapter and its focused tests
> are library-level inputs only. No native, Isaac/ROS, or real-robot product
> launcher currently wires it to a qualified runtime owner. The relevant
> execution and lane gates are I6/N, I9/I, and I10/R.

`manyforge_ros2_control_execution` is the generic C++ ROS 2 execution
adapter. It sends `FollowJointTrajectory` goals, observes controller and joint
state, implements bounded stop/reset behavior, and projects observed state
without placing ROS types in `manyforge_core`.

## Relocation responsibility split

The retained I6/I7/I8 inputs are exact controller and joint binding checks,
controller-manager discovery, ROS action observation/cancellation, bounded NRT
queues, generation/freshness fencing, and the controller-specific anchored
FollowJointTrajectory Stop mechanics. They remain C++ porting and acceptance
material; no installed executable, product factory, or launch path constructs
this backend today.

The current `manyforge.ros2_control.execution_binding.v0.experimental`
lifecycle-admission projection, singleton robot/group identities, old
environment epoch pinning, command-gate topic, and backend-local scene,
simulation, and gripper semantics are pre-relocation inputs rather than the
new contract. I6 must consume addressed Program Binding, I7 must supply the
new lifecycle facts, and I8 must place Stop policy behind the Kernel owner
before a product route is restored.

In the captured pre-relocation contract, physical deployments could
additionally enable an exact controller-binding and lifecycle-admission gate.
The gate was robot-family-neutral: xArm, UR, or another adapter resolved
vendor-specific names and lifecycle semantics before they reached this
package.

## Exact binding contract

The captured physical deployment contract supplied
`execution.config.controller_binding` using schema
`manyforge.ros2_control.execution_binding.v0.experimental`. The immutable
contract identifies:

- the controller manager and its reliable, transient-local activity topic;
- the hardware component;
- the trajectory controller's manager-local name, ROS name, type, action, and
  state topic;
- the joint-state topic and exact required joint order;
- the exact claimed command and required state interfaces; and
- the maximum age of one verified binding snapshot.

The configured action, state topics, and joint order must agree with the
ordinary execution configuration. Empty fields, duplicate names, an unknown
schema, and an inexact contract fail configuration.

The controller monitor runs outside the controller update loop. It observes
the initial activity generation, queries `ListControllers` and
`ListHardwareComponents`, then verifies one exact active controller, one
active hardware component, type, claims, state-interface availability, and
absence of conflicting claims. An activity event invalidates the old
snapshot before an asynchronous refresh begins. The event generation before
and after the two service reads fences manager changes during that refresh.

## Lifecycle admission

The captured contract set
`execution.config.require_execution_admission_snapshot: true` for a physical
deployment requiring lifecycle authority. Its selected robot-lifecycle
provider published immutable `ExecutionAdmissionSnapshot` values through the
former native bundle:

- `publish_ros2_control_execution_admission_snapshot(...)`;
- `get_ros2_control_execution_admission_snapshot()`; and
- `get_ros2_control_controller_binding_snapshot()`.

Each admission binds a robot identity and exact binding generation to fresh
controller-readiness and motion-admission facts. Publication rejects invalid
timestamps, regressing generations, changing identity, duplicate reason
codes, or a semantic change without a new lifecycle generation. A publication
may renew timestamps and its lease within the same lifecycle generation, but
may not silently change readiness or admission.

The captured xArm runtime manifest carried this generic binding. Retained
artifact tooling preserves its validation inputs, but product wiring that
projects lifecycle authority into admission snapshots remains a separate
real-robot integration gate.

## Dispatch and runtime behavior

Immediately before the action client enqueues a goal, one bounded
linearization point reads the immutable controller and lifecycle snapshots.
It performs no controller-manager call, YAML parsing, model work, or wait. A
missing, stale, invalid, mismatched, or non-admitted snapshot rejects the goal
before `async_send_goal`.

An accepted goal pins:

- the binding contract and controller-manager activity generation; and
- when required, the lifecycle generation.

The ordinary NRT execution monitor rechecks fresh immutable publications. A
controller activity change, invalid/stale binding, lifecycle-generation
change, or lost admission cancels the known goal and fails the execution
closed. A delayed goal response is cancelled if it arrives after its
execution generation lost authority.

The historical contract allowed non-physical deployments without
`controller_binding`; that statement is not current simulation availability.
Requiring lifecycle admission without an exact binding remains invalid
configuration for the future consumer.

## Concurrency and observability

Controller-manager service work runs in one NRT worker. Snapshot readers use
immutable atomic publications. The dispatch mutex only serializes publication
with the final goal-enqueue boundary; it is never used by a controller update
loop.

Controller progress metrics expose activity events, refresh
attempts/successes/failures, binding and admission dispatch rejections, and
authority losses. Stable failure reasons distinguish controller-binding
rejection/loss from lifecycle-admission rejection/loss.

## Qualification boundary

The retained deterministic tests cover configuration validation, the admission
matrix, generation and lease rules, initial activity fencing, missing
manager/services, inactive or conflicting controllers, restart churn,
pre-dispatch rejection, authority loss during execution, and delayed action
responses. The captured xArm fake/HIL qualification additionally loaded the
exact generated Lite6 contract in a real Jazzy `controller_manager` and
`joint_trajectory_controller`.

When they pass in the approved container, these focused tests can qualify only
the retained generic consumer substrate. They do not qualify a physical
trajectory, a vendor lifecycle transition, production lifecycle-to-admission
launcher wiring, or a public operator/PLC ingress.
