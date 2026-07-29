# ManyForge Robot Lifecycle

`manyforge_robot_lifecycle` is the robot-family-neutral, non-real-time
supervision core for real-robot lifecycle work. It deliberately contains no
ROS, xArm, Universal Robots, Composer, PLC, or behavior-tree type.

The library owns:

- one immutable authority-profile selection per supervisor instance;
- orthogonal observed facts and fail-closed readiness derivation;
- stable robot-neutral target intents such as `robot.prepare_for_motion` and
  `robot.make_noncommandable`;
- generation-fenced authority sessions and snapshots;
- capability-driven command admission;
- bounded, ordered, idempotent request handling;
- explicit transaction phases, deadlines, and uncertain-outcome fencing; and
- immutable snapshots suitable for thin ROS and backend wrappers.

`AdmissionProjector` converts those snapshots into a robot-neutral,
independently versioned NRT publication. A semantic lifecycle generation
changes only when readiness, admission, reasons, authority, or provider
identity changes; publication generations still advance for lease renewals.
This distinction is required because observation freshness can expire without
changing the reducer's source generation. `AdmissionPublicationDriver`
refreshes one `LifecycleControlPort`, projects the resulting snapshot, and
publishes an explicit closed state after a representable refresh failure. If
projection or transport fails, the consumer's earlier bounded lease expires.

The matching ROS wire type lives in the IDL-only
`manyforge_robot_lifecycle_msgs` package. It carries a relative lease and a
source steady-clock timestamp; a consumer stamps receipt locally and never
extends the source deadline. This contract is for separate processes on the
same Linux host, where `CLOCK_MONOTONIC` has one boot epoch even across
ordinary containers; it is not a distributed-clock protocol. The publication
layer remains NRT and contains no robot-family semantics.

`LocalOperatorGateway` is the headless, robot-neutral mutation boundary. It
does not authenticate credentials. Instead, a trusted HTTP, mTLS, or other
ingress supplies a short-lived `AuthenticatedPrincipal` assertion. The gateway
then:

- requires the lifecycle-operator role for every mutation;
- binds robot identity, `local_operator` profile, principal, authentication
  context, and authority session on the server side rather than accepting them
  in command payloads;
- rejects sessions that outlive authentication or the configured lease cap;
- prevents another principal or authentication context from replacing, using,
  or closing an active session; and
- records bounded audit facts without credentials or bearer secrets.

Continuous observation delivery renews freshness without changing the
confirmed semantic snapshot generation. A changed observation, a discontinuity
after lease expiry, or any authority/transaction transition still advances the
generation. This keeps operator confirmation usable without allowing a stale
or materially changed state to bypass optimistic fencing.

The `LifecycleControlPort` is implemented by the one transport-owning adapter.
The gateway neither obtains that transport nor calls a controller update loop.
Profile-specific ingress and authentication remain outside this ROS-independent
library.

Adapters may explicitly mark an attempted primitive as outcome-unknown and
must reconcile it from observation; they cannot downgrade uncertainty to an
ordinary failure. The allocation-free `motion_admitted(now_ns)` query exists
for an NRT owner to publish a bounded admission lease to a real-time consumer.
It does not make the reducer thread-safe.

The library never calls a robot, controller manager, frontend, or PLC. A
dedicated robot adapter owns the vendor transition graph, performs accepted
side effects outside controller update loops, verifies observed
postconditions, and reports transaction progress back to this core.

Composer and PLC/ROXSIE integrations are clients of the same target-intent
contract. They request one durable target transaction and render its result;
they do not choose, loop over, or retry vendor primitives. The selected
adapter may advance a target through vendor services and controller-manager
operations, but it must prove each stage from a fresh observation before
issuing another. An already-satisfied target succeeds without a physical
command. An uncertain stage fences the transaction for explicit
reconciliation and is never automatically repeated.

Cause-specific recovery and controller shutdown remain separate capabilities.
Preparing a robot for motion does not dispatch a trajectory or clear a fault,
and making it non-commandable does not shut down the robot controller.

Composer's current optional shared bearer token is not, by itself, sufficient
to mint an industrial lifecycle-operator assertion: its `X-Principal-ID` is
caller-selected after possession of the shared token. No Composer or ROS
mutation endpoint is enabled by this package. A production ingress must bind
identity and role cryptographically or through an equivalently trusted local
authority before constructing `AuthenticatedPrincipal`.

This package is an internal XC-7A implementation boundary. It does not expose
a public deployment schema or claim that a physical robot lifecycle has been
qualified.
