# ManyForge Supervision

`manyforge_supervision.runtime_observation` is a bounded ROS-free reducer
foundation for composable runtime-unit observation. It keeps process
presence/incarnation, source-attested health/progress, and Manager-derived
freshness/dependency state separate. It operates on caller-supplied,
already-qualified internal evidence; it does not yet qualify adapter/product
identities, prove retained-bundle membership or staged readiness, or implement
final I4 Manager convergence. The reducer performs no process effect, Apply,
active-pointer mutation, or robot-readiness inference. Its immutable values
remain an internal model until I3/I4 freeze the public progress wire contract.

`manyforge_supervision.component_health` and
`manyforge_supervision.manifest` form the ROS-free, vendor-neutral domain
layer for component heartbeats, dependency-health reduction, strict manifest
translation, and immutable execution-environment pins.

The domain layer performs no ROS I/O, process control, robot command, logging,
or sleeping. Deployment adapters provide receiver-local evidence and apply
the reducer's containment/restart decision. The physical execution adapter
keeps its own lower-latency command and dispatch fences.

`manyforge_supervision.cell_supervisor` is a separate executable adapter for
the current Docker deployment. It lives outside Composer and the robot-control
owner, never sends a robot command, and applies only bounded,
dependency-ordered process teardown. Its arm/shutdown files live in a
host-only control directory; containers receive the heartbeat/evidence path,
not process-control write authority.

Container `running` state is not treated as component health. After a bounded
startup grace, the host supervisor consumes Composer's already-reduced
`cellSupervision` snapshot over the NRT runtime-status channel. It pins the
Composer and cell-supervisor epochs and renews receiver-local freshness only
when the reduced snapshot itself progresses. A frozen endpoint, exact replay,
epoch replacement, unhealthy dependency graph, or container exit is terminal
for that launcher generation: Composer is stopped before the owner, and the
operator must start a fresh observation-only lane. The supervisor never
restarts, re-enables, or resumes the lane automatically.
