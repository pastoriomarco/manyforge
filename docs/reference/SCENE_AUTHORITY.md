# Live Scene Authority Contract

The live planning scene is execution state. It must not be reconciled from the
authored program draft by background UI effects or other undeclared writers.

## Declared Writers

Only these authorities may mutate the live planning scene:

- Program behavior-tree scene nodes during a cycle.
- Explicit Reset, which returns the runtime scene to the authored baseline.
- Explicit program-load seeding, which populates authored objects for idle
  visualization without building or resuming a behavior tree.
- Explicit operator/API scene edits when the lifecycle guard says mutation is
  safe.
- Configured pose-source feeds, under their deployment policy and safety gates.

Any other live-scene writer is a defect unless it is added to this contract and
covered by tests.

## Authored Draft vs Runtime Truth

The authored program scene is design intent. It is applied on load, Reset, and
Run preparation. It is not continuously mirrored into the live scene while the
runtime is idle or stopped.

Pose sources are different: they are declared runtime-truth authorities. A pose
source may update configured object poses from observed world state so planning
uses the cell as it is, not stale authored fiction. Pose sources are opt-in in
deployment files, freshness-checked, evented, and blocked while a stopped cycle
still has a resumable behavior-tree context.

## Attachment Policy

Use `continuous_unattached` for graspable objects that should follow external
observations only while detached. This is the recommended policy for typical
pick-and-place parts because it cannot fight a held object.

The `continuous` policy is more permissive: when an object is attached, it may
update the attachment-relative offset from the observation. Keep it only for
deployments that intentionally want that behavior. Do not use bare `continuous`
for graspable parts as a default.

Pose sources update poses only. They must not delete, recreate, or reset objects
unless a future authority is explicitly designed, documented, and tested.

