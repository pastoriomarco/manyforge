# ManyForge ros2_control execution

`manyforge_ros2_control_execution` is the generic C++ ROS 2 execution
adapter. It sends `FollowJointTrajectory` goals, observes controller and joint
state, implements bounded stop/reset behavior, and projects observed state
without placing ROS types in `manyforge_core`.

Physical deployments can additionally enable an exact controller-binding and
lifecycle-admission gate. The gate is robot-family-neutral: xArm, UR, or
another adapter must resolve vendor-specific names and lifecycle semantics
before they reach this package.

## Exact binding contract

A physical deployment supplies
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

Set `execution.config.require_execution_admission_snapshot: true` for a
physical deployment that requires lifecycle authority. The selected
robot-lifecycle provider publishes immutable
`ExecutionAdmissionSnapshot` values through the native bundle:

- `publish_ros2_control_execution_admission_snapshot(...)`;
- `get_ros2_control_execution_admission_snapshot()`; and
- `get_ros2_control_controller_binding_snapshot()`.

Each admission binds a robot identity and exact binding generation to fresh
controller-readiness and motion-admission facts. Publication rejects invalid
timestamps, regressing generations, changing identity, duplicate reason
codes, or a semantic change without a new lifecycle generation. A publication
may renew timestamps and its lease within the same lifecycle generation, but
may not silently change readiness or admission.

The current xArm adapter emits the generic binding in its resolved runtime
manifest. Product launch wiring that continuously projects the selected
lifecycle authority into these admission snapshots remains a separate
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

Deployments without `controller_binding` preserve the existing simulation and
non-physical behavior. Requiring lifecycle admission without an exact binding
is invalid configuration.

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

The deterministic tests cover configuration validation, the admission
matrix, generation and lease rules, initial activity fencing, missing
manager/services, inactive or conflicting controllers, restart churn,
pre-dispatch rejection, authority loss during execution, and delayed action
responses. The xArm fake/HIL lane additionally loads the exact generated
Lite6 contract in a real Jazzy `controller_manager` and
`joint_trajectory_controller`.

These tests qualify the generic consumer and fake controller path. They do
not qualify a physical trajectory, a vendor lifecycle transition, the
production lifecycle-to-admission launcher wiring, or a public
operator/PLC ingress.
