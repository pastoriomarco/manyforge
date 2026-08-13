# ManyForge xArm control

`manyforge_xarm_control` is the thin ROS adapter for the build-owned Lite6
brand-lifecycle product. It does not own CCLC transactions, caller authority,
readiness, execution admission, retry policy, or Stop semantics.

## Current boundary

The package contains three production pieces:

- the generated Lite6 lifecycle product identity and canonical lifecycle plan
  catalog;
- `Lite6BrandLifecyclePort`, which implements Core's
  `IBrandLifecyclePort` for one exact retained provider occurrence; and
- `XArmRosTransportSession`, an NRT client of the restricted services and
  supervised state published by the sole `xarm_ros2` transport owner.

CCLC selects and sequences catalog steps, owns deadlines and reconciliation,
and supplies the exact provider/runtime occurrence. The adapter validates that
identity, projects fresh vendor observations, and translates only the closed
catalog primitive set. It never accepts raw SDK mode, state, servo, or
system-control arguments.

```text
CCLC transaction owner
  -> IBrandLifecyclePort
  -> Lite6BrandLifecyclePort
  -> XArmRosTransportSession
  -> restricted xarm_ros2 ROS services
  -> sole xarm_ros2 SDK/session owner
```

One physical request is dispatched at most once. A timeout after dispatch is
reported as outcome-unknown and requires CCLC reconciliation; it is never
retried by this adapter. A supervised source-session replacement invalidates
the transport object and requires reconstruction.

The low-level renewable `set_command_gate` transport call remains only as an
explicit I10 replacement seam. It is not an I7 caller/session authority and is
not exposed through a public lifecycle API.

## Removed legacy authority

I7/L0.6 removed the former generic lifecycle supervisor, caller authority
profiles/sessions, local-operator gateway, admission publisher, fake hardware
plugin, and the unavailable compatibility executables. Their historical
package names and entrypoints are not supported aliases. Current clients use
the CCLC request/status APIs and the `manyforge_cclc_msgs` transport package.

`shutdown_controller` remains a distinct, confirmation-bound transport
primitive. The adapter maps it only to the dedicated supervised shutdown
service; it cannot be smuggled through an ordinary lifecycle command value.
Physical invocation remains subject to the attended I10 qualification gate.

## Qualification

The focused package tests cover:

- canonical generated catalog/product identity and exact provider occurrence
  joins;
- observation mapping, stale evidence, and source-session replacement;
- exact named lifecycle dispatch with no automatic retry;
- timeout-after-dispatch uncertainty;
- the retained low-level command-gate seam; and
- the dedicated confirmed shutdown path.

From the maintained real-Lite6 image:

```bash
./scripts/ros/build-real-lite6-runtime.sh
MANYFORGE_SKIP_REAL_LITE6_BUILD=1 \
  ./scripts/ros/test-xarm-lifecycle-fake.sh
```

These are offline adapter/transport checks. They do not claim a physical robot
command, motion qualification, or production I10 composition.
